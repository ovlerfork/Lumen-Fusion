/**
 * @file src/platform/macos/login_item.mm
 * @brief Native macOS Launch at Login settings backed by SMAppService.
 */

#include "src/login_item.h"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include <string>

namespace {
  bool prefers_chinese() {
    NSString *language = [[NSLocale preferredLanguages] firstObject];
    return [language hasPrefix:@"zh"];
  }

}  // namespace

namespace login_item {
  bool native_is_durable_app_install() {
    @autoreleasepool {
      NSURL *bundle_url = [[NSBundle mainBundle] bundleURL];
      if (bundle_url == nil || ![bundle_url isFileURL]) return false;
      return durable_app_path([[bundle_url path] UTF8String], [NSHomeDirectory() UTF8String]);
    }
  }

  status native_status() {
    status result = status::error;
    @autoreleasepool {
      if (@available(macOS 13.0, *)) {
        switch ([[SMAppService mainAppService] status]) {
          case SMAppServiceStatusEnabled:
            result = status::enabled;
            break;
          case SMAppServiceStatusRequiresApproval:
            result = status::requires_approval;
            break;
          case SMAppServiceStatusNotRegistered:
            result = status::not_registered;
            break;
          case SMAppServiceStatusNotFound:
            result = status::not_found;
            break;
          default:
            result = status::error;
            break;
        }
      } else {
        result = status::unavailable;
      }
    }
    return result;
  }

  bool native_register(std::string &message) {
    @autoreleasepool {
      if (!native_is_durable_app_install()) {
        message = "Drag Lumen Fusion.app to Applications, then reopen this setting.";
        return false;
      }
      if (@available(macOS 13.0, *)) {
        NSError *error = nil;
        const BOOL succeeded = [[SMAppService mainAppService] registerAndReturnError:&error];
        if (!succeeded && error != nil) {
          const char *details = [[error localizedDescription] UTF8String];
          if (details != nullptr) message = details;
        }
        return succeeded;
      }
      message = "Launch at Login requires macOS 13 or later.";
      return false;
    }
  }

  bool native_unregister(std::string &message) {
    @autoreleasepool {
      if (@available(macOS 13.0, *)) {
        NSError *error = nil;
        const BOOL succeeded = [[SMAppService mainAppService] unregisterAndReturnError:&error];
        if (!succeeded && error != nil) {
          const char *details = [[error localizedDescription] UTF8String];
          if (details != nullptr) message = details;
        }
        return succeeded;
      }
      message = "Launch at Login requires macOS 13 or later.";
      return false;
    }
  }

  void native_open_login_items() {
    if (@available(macOS 13.0, *)) {
      [SMAppService openSystemSettingsLoginItems];
    }
  }
}  // namespace login_item

@interface LoginItemPanelController : NSObject <NSWindowDelegate> {
    NSPanel *_panel;
    NSButton *_checkbox;
    NSTextField *_status;
    NSButton *_approval_button;
    NSButton *_unregister_button;
  }
  - (void)showPanel;
  @end

static LoginItemPanelController *active_panel_controller = nil;

@implementation LoginItemPanelController
  - (id)init {
    self = [super init];
    if (self == nil) {
      return nil;
    }

    const bool chinese = prefers_chinese();
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 250)
                                        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    [_panel setTitle:chinese ? @"登录时启动" : @"Launch at Login"];
    [_panel setReleasedWhenClosed:NO];
    [_panel setDelegate:self];

    NSView *content = [_panel contentView];
    NSTextField *description = [NSTextField labelWithString:chinese ? @"Lumen Fusion 会在你下次登录用户账户时启动，不会在系统开机时启动。" : @"Lumen Fusion starts at your next user login, not at system boot."];
    [description setFrame:NSMakeRect(24, 197, 412, 40)];
    [description setUsesSingleLineMode:NO];
    [[description cell] setWraps:YES];
    [content addSubview:description];

    _checkbox = [[NSButton alloc] initWithFrame:NSMakeRect(20, 164, 412, 26)];
    [_checkbox setButtonType:NSSwitchButton];
    [_checkbox setTitle:chinese ? @"登录时启动 Lumen Fusion" : @"Launch Lumen Fusion at login"];
    [_checkbox setTarget:self];
    [_checkbox setAction:@selector(checkboxChanged:)];
    [content addSubview:_checkbox];

    _status = [[NSTextField labelWithString:@""] retain];
    [_status setFrame:NSMakeRect(24, 65, 412, 88)];
    [_status setTextColor:[NSColor secondaryLabelColor]];
    [_status setUsesSingleLineMode:NO];
    [[_status cell] setWraps:YES];
    [_status setSelectable:YES];
    [content addSubview:_status];

    _approval_button = [[NSButton alloc] initWithFrame:NSMakeRect(24, 20, 190, 32)];
    [_approval_button setTitle:chinese ? @"打开“登录项”设置" : @"Open Login Items Settings"];
    [_approval_button setTarget:self];
    [_approval_button setAction:@selector(openApproval:)];
    [content addSubview:_approval_button];

    _unregister_button = [[NSButton alloc] initWithFrame:NSMakeRect(230, 20, 170, 32)];
    [_unregister_button setTitle:chinese ? @"取消启动项" : @"Remove Login Item"];
    [_unregister_button setTarget:self];
    [_unregister_button setAction:@selector(unregister:)];
    [content addSubview:_unregister_button];

    return self;
  }

  - (void)dealloc {
    [_checkbox release];
    [_status release];
    [_approval_button release];
    [_unregister_button release];
    [_panel setDelegate:nil];
    [_panel release];
    [super dealloc];
  }

  - (void)applyState:(const login_item::view_state &)state {
    const bool chinese = prefers_chinese();
    [_checkbox setState:state.checkbox_checked ? NSControlStateValueOn : NSControlStateValueOff];
    [_checkbox setEnabled:state.checkbox_enabled];
    [_approval_button setHidden:!state.show_approval_button];
    [_unregister_button setHidden:!state.show_unregister_button];
    NSString *message = [NSString stringWithUTF8String:state.message.c_str()];
    if (chinese && state.message == "Drag Lumen Fusion.app to Applications, then reopen this setting.") {
      message = @"请先将 Lumen Fusion 拖到“应用程序”文件夹，然后重新打开此设置。";
    } else if (chinese && state.message == login_item::status_description(login_item::status::enabled)) {
      message = @"已启用，将在下次登录时启动。";
    } else if (chinese && state.message == login_item::status_description(login_item::status::requires_approval)) {
      message = @"等待你在“登录项”中批准，或可取消此启动项。";
    } else if (chinese && state.message == login_item::status_description(login_item::status::not_registered)) {
      message = @"未启用。";
    } else if (chinese && state.message == login_item::status_description(login_item::status::not_found)) {
      message = @"系统未找到此登录项。";
    } else if (chinese && state.message == login_item::status_description(login_item::status::unavailable)) {
      message = @"此功能需要 macOS 13 或更高版本。";
    } else if (chinese && state.message == login_item::status_description(login_item::status::error)) {
      message = @"无法读取登录项状态。";
    }
    [_status setStringValue:message ?: @""];
    [_status setToolTip:message];
  }

  - (void)refresh {
    login_item::settings_controller settings;
    [self applyState:settings.refresh(login_item::native_is_durable_app_install())];
  }

  - (void)checkboxChanged:(id)sender {
    const bool enabled = [(NSButton *) sender state] == NSControlStateValueOn;
    login_item::settings_controller settings;
    [self applyState:settings.set_enabled(enabled, login_item::native_is_durable_app_install())];
  }

  - (void)openApproval:(id)sender {
    login_item::settings_controller settings;
    [self applyState:settings.open_approval(login_item::native_is_durable_app_install())];
  }

  - (void)unregister:(id)sender {
    login_item::settings_controller settings;
    [self applyState:settings.set_enabled(false, login_item::native_is_durable_app_install())];
  }

  - (void)showPanel {
    [self refresh];
    [_panel center];
    [_panel makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
  }

  - (void)windowDidBecomeKey:(NSNotification *)notification {
    [self refresh];
  }

  - (void)windowWillClose:(NSNotification *)notification {
    if (active_panel_controller == self) {
      active_panel_controller = nil;
      [self autorelease];
    }
  }
@end

namespace login_item {
  void show_settings_panel() {
    dispatch_async(dispatch_get_main_queue(), ^{
      @autoreleasepool {
        if (active_panel_controller == nil) {
          active_panel_controller = [[LoginItemPanelController alloc] init];
        }
        [active_panel_controller showPanel];
      }
    });
  }
}  // namespace login_item
