#include "src/adaptive_display.h"
#include "src/logging.h"
#include "virtual_display.h"

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <vector>

namespace adaptive_display {
  namespace {
    IOPMAssertionID retention_assertion = kIOPMNullAssertionID;
    std::string assertion_kind = "none";

    topology inspect(uint32_t own_display) {
      topology result;
      if (auto info = IOPSCopyPowerSourcesInfo()) {
        auto source = IOPSGetProvidingPowerSourceType(info);
        if (source) {
          result.power = CFEqual(source, CFSTR(kIOPSACPowerValue)) ? power_source::external :
                         CFEqual(source, CFSTR(kIOPSBatteryPowerValue)) ? power_source::battery : power_source::unknown;
        }
        CFRelease(info);
      }

      std::optional<bool> lid_closed;
      auto root = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
      if (root) {
        auto value = IORegistryEntryCreateCFProperty(root, CFSTR("AppleClamshellState"), kCFAllocatorDefault, 0);
        if (value) {
          if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
            lid_closed = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
          }
          CFRelease(value);
        }
        IOObjectRelease(root);
      }

      uint32_t count = 0;
      if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess) {
        return result;
      }
      std::vector<CGDirectDisplayID> displays(count);
      if (count && CGGetOnlineDisplayList(count, displays.data(), &count) != kCGErrorSuccess) {
        return result;
      }
      bool uncertain = false;
      for (uint32_t i = 0; i < count; ++i) {
        auto id = displays[i];
        if (id == own_display || !CGDisplayIsOnline(id) || !CGDisplayIsActive(id) || CGDisplayIsAsleep(id)) {
          continue;
        }
        if (CGDisplayIsBuiltin(id)) {
          if (!lid_closed) {
            uncertain = true;
            continue;
          }
          if (*lid_closed) {
            continue;
          }
        }
        auto bounds = CGDisplayBounds(id);
        if (bounds.size.width <= 0 || bounds.size.height <= 0) {
          continue;
        }
        result.local = presence::present;
        if (!result.local_main || id == CGMainDisplayID()) {
          result.local_main = id;
        }
      }
      if (result.local != presence::present) {
        result.local = uncertain ? presence::unknown : presence::absent;
      }
      return result;
    }

    void retention_power(const std::string &kind) {
      if (kind == assertion_kind) {
        return;
      }
      // Acquire the replacement before releasing the old assertion.
      IOPMAssertionID replacement = kIOPMNullAssertionID;
      if (kind != "none") {
        auto type = kind == "system" ? kIOPMAssertionTypePreventUserIdleSystemSleep : kIOPMAssertionTypePreventUserIdleDisplaySleep;
        auto result = IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn, CFSTR("Lumina paused virtual desktop"), &replacement);
        if (result != kIOReturnSuccess) {
          BOOST_LOG(warning) << "Could not acquire paused-desktop idle assertion: " << result;
          replacement = kIOPMNullAssertionID;
        }
      }
      if (retention_assertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(retention_assertion);
      }
      retention_assertion = replacement;
      assertion_kind = replacement == kIOPMNullAssertionID ? "none" : kind;
    }
  }

  backend macos_backend() {
    return {
      inspect,
      [](mode m, const char *layout) { return virtual_display_ensure(m.width, m.height, m.fps, layout); },
      [](const char *layout, uint32_t local) { return virtual_display_apply_layout(layout, local) != 0; },
      [](uint32_t id) {
        auto target = virtual_display_get_target_id();
        auto bounds = CGDisplayBounds(target);
        return id && virtual_display_get_id() == id && target && bounds.size.width > 0 && bounds.size.height > 0;
      },
      [] { virtual_display_destroy(); },
      retention_power
    };
  }

  void show_status_panel() {
    const auto s = snapshot();
    NSString *message = !s.display ? @"No retained virtual desktop." :
                        s.preparing || s.active ? @"Virtual desktop is in use. Release is available after clients disconnect." :
                        s.deadline ? @"Virtual desktop retained for Resume until its retention deadline." :
                                     @"Virtual desktop retained for Resume until Quit.";
    NSAlert *panel = [[NSAlert alloc] init];
    panel.messageText = @"Virtual Desktop";
    panel.informativeText = message;
    [panel addButtonWithTitle:@"Close"];
    if (s.display && !s.preparing && !s.active) {
      [panel addButtonWithTitle:@"Release Desktop"];
    }
    if ([panel runModal] == NSAlertSecondButtonReturn && !release_inactive()) {
      NSAlert *busy = [[NSAlert alloc] init];
      busy.messageText = @"Virtual desktop is now in use.";
      [busy runModal];
    }
  }
}  // namespace adaptive_display
