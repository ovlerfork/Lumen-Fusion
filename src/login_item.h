/**
 * @file src/login_item.h
 * @brief State handling for the macOS Launch at Login settings panel.
 */
#pragma once

#include <iosfwd>
#include <string>

#ifdef SUNSHINE_TESTS
  #include <functional>
  #include <utility>
#endif

namespace login_item {
  enum class status {
    enabled,
    requires_approval,
    not_registered,
    not_found,
    unavailable,
    error,
  };

  struct view_state {
    status service_status;
    bool checkbox_checked;
    bool checkbox_enabled;
    bool show_approval_button;
    bool show_unregister_button;
    std::string message;
    bool action_failed {false};
  };

  class settings_controller {
  public:
    settings_controller() = default;

#ifdef SUNSHINE_TESTS
    struct test_api {
      std::function<status()> current_status;
      std::function<bool(std::string &)> register_service;
      std::function<bool(std::string &)> unregister_service;
      std::function<void()> open_system_settings_login_items;
    };
    explicit settings_controller(test_api api): test_api_ {std::move(api)} {}
#endif

    [[nodiscard]] view_state refresh(bool installed_in_applications) const;
    [[nodiscard]] view_state set_enabled(bool enabled, bool installed_in_applications);
    [[nodiscard]] view_state open_approval(bool installed_in_applications);

  private:
    [[nodiscard]] view_state state_for(status service_status, bool installed_in_applications, std::string message = {}, bool action_failed = false) const;
    [[nodiscard]] status current_status() const;
    bool register_service(std::string &error);
    bool unregister_service(std::string &error);
    void open_system_settings_login_items();

#ifdef SUNSHINE_TESTS
    test_api test_api_ {};
#endif
  };

  [[nodiscard]] const char *status_description(status service_status);
  [[nodiscard]] const char *status_token(status service_status);
  int command(const char *name, int argc, char *argv[]);

#ifdef SUNSHINE_TESTS
  int command(const char *name, int argc, char *argv[], settings_controller &settings, bool installed_in_applications, std::ostream &output, std::ostream &errors);
#endif

#if defined(__APPLE__) || defined(__MACH__)
  void show_settings_panel();
#endif

  // The native implementation reports the current OS state on each call.
  [[nodiscard]] status native_status();
  bool native_register(std::string &error);
  bool native_unregister(std::string &error);
  void native_open_login_items();
  [[nodiscard]] bool native_is_durable_app_install();
  [[nodiscard]] bool durable_app_path(const std::string &bundle_path, const std::string &home_path);
}  // namespace login_item
