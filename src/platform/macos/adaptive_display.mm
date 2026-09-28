#include "src/adaptive_display.h"
#include "src/logging.h"
#include "virtual_display.h"

#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace adaptive_display {
  namespace {
    IOPMAssertionID retention_assertion = kIOPMNullAssertionID;
    std::string assertion_kind = "none";

    using display_list_reader = std::function<CGError(uint32_t, CGDirectDisplayID *, uint32_t *)>;

    std::optional<std::vector<CGDirectDisplayID>> read_display_list(const display_list_reader &read) {
      uint32_t expected = 0, fetched = 0, confirmed = 0;
      if (read(0, nullptr, &expected) != kCGErrorSuccess || expected == std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
      }
      // One spare entry exposes growth that a count-sized buffer could truncate.
      std::vector<CGDirectDisplayID> ids(expected + 1);
      if (read(static_cast<uint32_t>(ids.size()), ids.data(), &fetched) != kCGErrorSuccess || fetched != expected ||
          read(0, nullptr, &confirmed) != kCGErrorSuccess || confirmed != fetched) {
        return std::nullopt;
      }
      ids.resize(fetched);
      std::sort(ids.begin(), ids.end());
      if ((!ids.empty() && ids.front() == 0) || std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return std::nullopt;
      }
      return ids;
    }

    struct display_lists {
      std::vector<CGDirectDisplayID> online, active;
      bool operator==(const display_lists &) const = default;
    };

    std::optional<display_lists> read_display_lists(const display_list_reader &online_reader, const display_list_reader &active_reader) {
      auto online = read_display_list(online_reader);
      auto active = read_display_list(active_reader);
      if (!online || !active || !std::includes(online->begin(), online->end(), active->begin(), active->end())) {
        return std::nullopt;
      }
      return display_lists {std::move(*online), std::move(*active)};
    }

    topology inspect(uint32_t own_display) {
      topology result;
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

      const auto lists = read_display_lists(CGGetOnlineDisplayList, CGGetActiveDisplayList);
      if (!lists) {
        return result;
      }
      const auto main = CGMainDisplayID();
      if ((main || !lists->active.empty()) && !std::binary_search(lists->active.begin(), lists->active.end(), main)) {
        return result;
      }
      bool uncertain = false;
      uint32_t local_main = 0;
      for (auto id : lists->online) {
        const bool active = std::binary_search(lists->active.begin(), lists->active.end(), id);
        // Scalar properties must agree with the complete lists, including our
        // own display. Contradictions indicate an in-progress reconfiguration.
        if (!CGDisplayIsOnline(id) || static_cast<bool>(CGDisplayIsActive(id)) != active) {
          return result;
        }
        if (id == own_display || !active || CGDisplayIsAsleep(id)) {
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
          return result;
        }
        if (!local_main || id == main) {
          local_main = id;
        }
      }
      const auto confirmed = read_display_lists(CGGetOnlineDisplayList, CGGetActiveDisplayList);
      if (!confirmed || *confirmed != *lists || main != CGMainDisplayID()) {
        return result;
      }
      result.local_main = local_main;
      result.main_display = main;
      result.local = local_main ? presence::present : uncertain ? presence::unknown : presence::absent;
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
          return;
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
      [](uint32_t id) { return id && virtual_display_get_id() == id; },
      [] { virtual_display_destroy(); },
      retention_power,
      [](uint32_t id) {
        const auto target = virtual_display_get_target_id();
        const auto bounds = CGDisplayBounds(target);
        return id && virtual_display_get_id() == id && target == id && bounds.size.width > 0 && bounds.size.height > 0;
      }
    };
  }

  void show_status_panel() {
    const auto s = snapshot();
    NSString *message = !s.display ? @"No retained virtual desktop." :
                        s.preparing || s.active ? @"Virtual desktop is in use. Release is available after clients disconnect." :
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

#ifdef SUNSHINE_TESTS
  #include "../../../tests/unit/platform/adaptive_display_topology_tests.h"
#endif
