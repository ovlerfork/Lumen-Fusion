#include "login_item.h"

#include <filesystem>
#include <iostream>
#include <ostream>
#include <string_view>
#include <utility>

namespace login_item {
  bool durable_app_path(const std::string &bundle_path, const std::string &home_path) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path bundle = fs::canonical(fs::path(bundle_path), error);
    if (error || bundle.extension() != ".app" || !fs::is_directory(bundle, error) || error) return false;
    for (const auto &part : bundle) {
      if (part == "AppTranslocation") return false;
    }
    if (bundle.string().starts_with("/Volumes/")) return false;

    const fs::path home = fs::canonical(fs::path(home_path), error);
    if (error) return false;
    const fs::path system_apps = "/Applications";
    const fs::path home_apps = home / "Applications";
    for (const fs::path &root : {system_apps, home_apps}) {
      const fs::path actual_root = fs::canonical(root, error);
      if (error || actual_root != root) {
        error.clear();
        continue;
      }
      const fs::path relative = bundle.lexically_relative(root);
      if (!relative.empty() && *relative.begin() != ".." && relative != ".") return true;
    }
    return false;
  }

#if !defined(__APPLE__) && !defined(__MACH__)
  status native_status() { return status::unavailable; }
  bool native_register(std::string &error) { error = "Requires macOS 13 or later"; return false; }
  bool native_unregister(std::string &error) { error = "Requires macOS 13 or later"; return false; }
  void native_open_login_items() {}
  bool native_is_durable_app_install() { return false; }
#endif

  status settings_controller::current_status() const {
#ifdef SUNSHINE_TESTS
    if (test_api_.current_status) return test_api_.current_status();
#endif
    return native_status();
  }

  bool settings_controller::register_service(std::string &error) {
#ifdef SUNSHINE_TESTS
    if (test_api_.register_service) return test_api_.register_service(error);
#endif
    return native_register(error);
  }

  bool settings_controller::unregister_service(std::string &error) {
#ifdef SUNSHINE_TESTS
    if (test_api_.unregister_service) return test_api_.unregister_service(error);
#endif
    return native_unregister(error);
  }

  void settings_controller::open_system_settings_login_items() {
#ifdef SUNSHINE_TESTS
    if (test_api_.open_system_settings_login_items) return test_api_.open_system_settings_login_items();
#endif
    native_open_login_items();
  }

  const char *status_description(status service_status) {
    switch (service_status) {
      case status::enabled:
        return "Enabled";
      case status::requires_approval:
        return "Approval required in Login Items";
      case status::not_registered:
        return "Not enabled";
      case status::not_found:
        return "Login item was not found";
      case status::unavailable:
        return "Requires macOS 13 or later";
      case status::error:
        return "Unable to read Login Items status";
    }
    return "Unable to read Login Items status";
  }

  const char *status_token(status service_status) {
    switch (service_status) {
      case status::enabled: return "enabled";
      case status::requires_approval: return "requiresApproval";
      case status::not_registered: return "notRegistered";
      case status::not_found: return "notFound";
      case status::unavailable: return "unavailable";
      case status::error: return "error";
    }
    return "error";
  }

  view_state settings_controller::state_for(status service_status, bool installed_in_applications, std::string message, bool action_failed) const {
    view_state result {
      .service_status = service_status,
      .checkbox_checked = service_status == status::enabled,
      .checkbox_enabled = installed_in_applications && service_status != status::unavailable,
      .show_approval_button = service_status == status::requires_approval,
      .show_unregister_button = service_status == status::requires_approval || service_status == status::enabled,
      .message = std::move(message),
      .action_failed = action_failed,
    };

    if (result.message.empty()) {
      result.message = installed_in_applications ? status_description(service_status) : "Drag Lumen Fusion.app to Applications, then reopen this setting.";
    }
    return result;
  }

  view_state settings_controller::refresh(bool installed_in_applications) const {
    return state_for(current_status(), installed_in_applications);
  }

  view_state settings_controller::set_enabled(bool enabled, bool installed_in_applications) {
    const auto before = current_status();
    if (enabled && !installed_in_applications) {
      return state_for(before, false, "Drag Lumen Fusion.app to Applications, then reopen this setting.", true);
    }

    if ((enabled && (before == status::enabled || before == status::requires_approval)) ||
        (!enabled && before == status::not_registered)) {
      return state_for(before, installed_in_applications);
    }

    if (before == status::error || before == status::unavailable) {
      return state_for(before, installed_in_applications, {}, true);
    }

    std::string error;
    const bool succeeded = enabled ? register_service(error) : unregister_service(error);
    const auto actual_status = current_status();
    if (!succeeded && error.empty()) {
      error = enabled ? "Could not enable Launch at Login." : "Could not disable Launch at Login.";
    }
    return state_for(actual_status, installed_in_applications, std::move(error), !succeeded);
  }

  view_state settings_controller::open_approval(bool installed_in_applications) {
    open_system_settings_login_items();
    return refresh(installed_in_applications);
  }

  namespace {
    int run_command(const char *name, int argc, char *argv[], settings_controller &settings, bool installed_in_applications, std::ostream &output, std::ostream &errors) {
      if (argc != 1 || argv == nullptr || argv[0] == nullptr) {
        errors << "Usage: " << name << " --login-item status|enable|disable\n";
        return 2;
      }

      const std::string_view action {argv[0]};
      view_state state;
      if (action == "status") {
        state = settings.refresh(installed_in_applications);
      } else if (action == "enable" || action == "disable") {
        state = settings.set_enabled(action == "enable", installed_in_applications);
      } else {
        errors << "Usage: " << name << " --login-item status|enable|disable\n";
        return 2;
      }

      output << "login-item status: " << status_token(state.service_status) << '\n';
      if (state.action_failed || state.service_status == status::error || state.service_status == status::unavailable ||
          (action != "status" && state.service_status == status::not_found)) {
        errors << "login-item error: "
               << (action == "status" ? status_description(state.service_status) : state.message) << '\n';
        return 1;
      }
      if (action == "enable" && state.service_status == status::not_registered) {
        errors << "login-item error: registration did not enable the item or request approval\n";
        return 1;
      }
      if (action == "disable" && state.service_status != status::not_registered) {
        errors << "login-item error: item remains registered\n";
        return 1;
      }
      return 0;
    }
  }  // namespace

  int command(const char *name, int argc, char *argv[]) {
    settings_controller settings;
    return run_command(name, argc, argv, settings, native_is_durable_app_install(), std::cout, std::cerr);
  }

#ifdef SUNSHINE_TESTS
  int command(const char *name, int argc, char *argv[], settings_controller &settings, bool installed_in_applications, std::ostream &output, std::ostream &errors) {
    return run_command(name, argc, argv, settings, installed_in_applications, output, errors);
  }
#endif
}  // namespace login_item
