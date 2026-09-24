#include <gtest/gtest.h>

#include "src/login_item.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace {
  struct fake_service {
    login_item::status current {login_item::status::not_registered};
    bool register_succeeds {true};
    login_item::status register_result {login_item::status::enabled};
    bool unregister_succeeds {true};
    int register_calls {0};
    int unregister_calls {0};
    int approval_calls {0};

    login_item::settings_controller controller() {
      return login_item::settings_controller {{
        .current_status = [this] { return current; },
        .register_service = [this](std::string &error) {
          ++register_calls;
          if (!register_succeeds) error = "Registration was rejected.";
          else current = register_result;
          return register_succeeds;
        },
        .unregister_service = [this](std::string &error) {
          ++unregister_calls;
          if (!unregister_succeeds) error = "Removal was rejected.";
          else current = login_item::status::not_registered;
          return unregister_succeeds;
        },
        .open_system_settings_login_items = [this] { ++approval_calls; },
      }};
    }
  };

  TEST(LoginItemSettingsTest, RefreshReadsExternalSystemState) {
    fake_service service;
    auto settings = service.controller();
    service.current = login_item::status::enabled;
    EXPECT_TRUE(settings.refresh(true).checkbox_checked);

    service.current = login_item::status::not_registered;
    const auto disabled = settings.refresh(true);
    EXPECT_FALSE(disabled.checkbox_checked);
    EXPECT_FALSE(disabled.show_unregister_button);
  }

  TEST(LoginItemSettingsTest, RepeatedActionsRespectCurrentSystemState) {
    fake_service service;
    auto settings = service.controller();

    EXPECT_EQ(settings.set_enabled(false, true).service_status, login_item::status::not_registered);
    EXPECT_EQ(service.unregister_calls, 0);

    service.current = login_item::status::enabled;
    EXPECT_TRUE(settings.set_enabled(true, true).checkbox_checked);
    EXPECT_EQ(service.register_calls, 0);

    service.current = login_item::status::requires_approval;
    const auto pending = settings.set_enabled(true, true);
    EXPECT_TRUE(pending.show_approval_button);
    EXPECT_EQ(service.register_calls, 0);

    service.current = login_item::status::not_registered;
    EXPECT_FALSE(settings.refresh(true).checkbox_checked);
    EXPECT_EQ(service.register_calls, 0);
  }

  TEST(LoginItemSettingsTest, ExplicitRegistrationCanAwaitApprovalThenReflectExternalChanges) {
    fake_service service;
    service.register_result = login_item::status::requires_approval;
    auto settings = service.controller();

    const auto pending = settings.set_enabled(true, true);
    EXPECT_EQ(service.register_calls, 1);
    EXPECT_FALSE(pending.checkbox_checked);
    EXPECT_TRUE(pending.show_approval_button);

    service.current = login_item::status::enabled;
    EXPECT_TRUE(settings.refresh(true).checkbox_checked);
    service.current = login_item::status::not_registered;
    EXPECT_FALSE(settings.refresh(true).checkbox_checked);
    EXPECT_EQ(service.register_calls, 1);
  }

  TEST(LoginItemSettingsTest, EnablingRequiresDurableApplicationsInstallBeforeRegistration) {
    fake_service service;
    auto settings = service.controller();

    const auto view = settings.set_enabled(true, false);

    EXPECT_EQ(service.register_calls, 0);
    EXPECT_FALSE(view.checkbox_checked);
    EXPECT_NE(view.message.find("Applications"), std::string::npos);
  }

  TEST(LoginItemSettingsTest, RegistrationFailureKeepsActualDisabledStatus) {
    fake_service service;
    service.register_succeeds = false;
    auto settings = service.controller();

    const auto view = settings.set_enabled(true, true);

    EXPECT_EQ(service.register_calls, 1);
    EXPECT_FALSE(view.checkbox_checked);
    EXPECT_EQ(view.service_status, login_item::status::not_registered);
    EXPECT_EQ(view.message, "Registration was rejected.");
  }

  TEST(LoginItemSettingsTest, PendingItemOffersApprovalAndCanBeRemoved) {
    fake_service service;
    service.current = login_item::status::requires_approval;
    auto settings = service.controller();

    const auto pending = settings.refresh(true);
    EXPECT_FALSE(pending.checkbox_checked);
    EXPECT_TRUE(pending.show_approval_button);
    EXPECT_TRUE(pending.show_unregister_button);
    (void) settings.open_approval(true);
    const auto removed = settings.set_enabled(false, true);

    EXPECT_EQ(service.approval_calls, 1);
    EXPECT_EQ(service.unregister_calls, 1);
    EXPECT_FALSE(removed.show_unregister_button);
    EXPECT_EQ(removed.service_status, login_item::status::not_registered);
  }

  TEST(LoginItemSettingsTest, FailedPendingRemovalPreservesPendingStatus) {
    fake_service service;
    service.current = login_item::status::requires_approval;
    service.unregister_succeeds = false;
    auto settings = service.controller();

    const auto view = settings.set_enabled(false, true);
    EXPECT_EQ(view.service_status, login_item::status::requires_approval);
    EXPECT_TRUE(view.show_approval_button);
    EXPECT_TRUE(view.show_unregister_button);
    EXPECT_EQ(view.message, "Removal was rejected.");
  }

  TEST(LoginItemSettingsTest, CommandReportsActualPendingAndExternalDenial) {
    fake_service service;
    service.register_result = login_item::status::requires_approval;
    auto settings = service.controller();
    std::ostringstream output;
    std::ostringstream errors;
    std::string action = "enable";
    char *argv[] = {action.data()};

    EXPECT_EQ(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(output.str(), "login-item status: requiresApproval\n");
    EXPECT_TRUE(errors.str().empty());
    EXPECT_EQ(service.register_calls, 1);

    output.str("");
    output.clear();
    EXPECT_EQ(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(service.register_calls, 1);

    service.current = login_item::status::not_registered;
    action = "status";
    argv[0] = action.data();
    output.str("");
    output.clear();
    EXPECT_EQ(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(output.str(), "login-item status: notRegistered\n");
    EXPECT_EQ(service.register_calls, 1);

    action = "disable";
    argv[0] = action.data();
    EXPECT_EQ(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(service.unregister_calls, 0);

    service.current = login_item::status::enabled;
    output.str("");
    output.clear();
    EXPECT_EQ(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(output.str(), "login-item status: notRegistered\n");
    EXPECT_EQ(service.unregister_calls, 1);
  }

  TEST(LoginItemSettingsTest, CommandReportsErrorsAndGuardsRegistration) {
    fake_service service;
    auto settings = service.controller();
    std::ostringstream output;
    std::ostringstream errors;
    std::string action = "enable";
    char *argv[] = {action.data()};

    EXPECT_NE(login_item::command("sunshine", 1, argv, settings, false, output, errors), 0);
    EXPECT_EQ(service.register_calls, 0);
    EXPECT_EQ(output.str(), "login-item status: notRegistered\n");
    EXPECT_NE(errors.str().find("Applications"), std::string::npos);

    output.str("");
    output.clear();
    errors.str("");
    errors.clear();
    service.register_succeeds = false;
    EXPECT_NE(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(output.str(), "login-item status: notRegistered\n");
    EXPECT_NE(errors.str().find("Registration was rejected."), std::string::npos);

    output.str("");
    output.clear();
    errors.str("");
    errors.clear();
    action = "unexpected";
    argv[0] = action.data();
    EXPECT_NE(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_TRUE(output.str().empty());
    EXPECT_EQ(errors.str(), "Usage: sunshine --login-item status|enable|disable\n");

    action = "status";
    argv[0] = action.data();
    service.current = login_item::status::error;
    output.str("");
    output.clear();
    errors.str("");
    errors.clear();
    EXPECT_NE(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(output.str(), "login-item status: error\n");
    EXPECT_NE(errors.str().find("Unable to read Login Items status"), std::string::npos);

    service.current = login_item::status::not_found;
    output.str("");
    output.clear();
    errors.str("");
    errors.clear();
    EXPECT_EQ(login_item::command("sunshine", 1, argv, settings, true, output, errors), 0);
    EXPECT_EQ(output.str(), "login-item status: notFound\n");
    EXPECT_TRUE(errors.str().empty());
  }

  TEST(LoginItemSettingsTest, RequiresActualApplicationsInstall) {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("lumen-login-item-" + std::to_string(stamp));
    const fs::path home = base / "home";
    const fs::path apps = home / "Applications";
    const fs::path external = base / "download";
    ASSERT_TRUE(fs::create_directories(apps / "Lumen Fusion.app"));
    ASSERT_TRUE(fs::create_directories(external / "Lumen Fusion.app"));
    EXPECT_TRUE(login_item::durable_app_path((apps / "Lumen Fusion.app").string(), home.string()));
    EXPECT_FALSE(login_item::durable_app_path((external / "Lumen Fusion.app").string(), home.string()));
#ifndef _WIN32
    fs::create_directory_symlink(external / "Lumen Fusion.app", apps / "Linked.app");
    EXPECT_FALSE(login_item::durable_app_path((apps / "Linked.app").string(), home.string()));
    fs::remove_all(apps);
    fs::create_directory_symlink(external, apps);
    EXPECT_FALSE(login_item::durable_app_path((apps / "Lumen Fusion.app").string(), home.string()));
#endif
    fs::remove_all(base);
  }

#if defined(__APPLE__) || defined(__MACH__)
  TEST(LoginItemSettingsTest, NativeStatusSmokeIsReadOnly) {
    const auto status = login_item::native_status();
    std::cout << "SMAppService.mainAppService status: " << login_item::status_description(status) << '\n';
    EXPECT_TRUE(status == login_item::status::enabled || status == login_item::status::requires_approval ||
                status == login_item::status::not_registered || status == login_item::status::not_found ||
                status == login_item::status::unavailable || status == login_item::status::error);
  }
#endif
}  // namespace
