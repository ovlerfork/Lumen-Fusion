/**
 * @file tests/unit/platform/test_macos_permissions.cpp
 * @brief Exercise permission transitions without changing the runner's TCC grants.
 */
#ifdef __APPLE__

#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <gtest/gtest.h>

#include "src/platform/common.h"
#include "src/platform/macos/misc.h"

namespace {
  struct permission_answers;
  thread_local permission_answers *active_answers = nullptr;

  struct permission_answers {
    permission_answers *previous = active_answers;
    bool allowed = false;
    bool allow_on_request = false;
    bool request_result = false;
    int requests = 0;

    permission_answers() {
      active_answers = this;
    }

    ~permission_answers() {
      active_answers = previous;
    }

    permission_answers(const permission_answers &) = delete;
    permission_answers &operator=(const permission_answers &) = delete;
  };
}  // namespace

// Only the test thread is overridden inside an explicit scope. Other tests and
// their worker threads continue to call CoreGraphics, without granting access.
extern "C" bool CGPreflightScreenCaptureAccess(void) {
  if (active_answers) {
    return active_answers->allowed;
  }
  static const auto original = reinterpret_cast<decltype(&CGPreflightScreenCaptureAccess)>(
    dlsym(RTLD_NEXT, "CGPreflightScreenCaptureAccess")
  );
  return original && original();
}

extern "C" bool CGRequestScreenCaptureAccess(void) {
  if (active_answers) {
    ++active_answers->requests;
    if (active_answers->allow_on_request) {
      active_answers->allowed = true;
    }
    return active_answers->request_result;
  }
  static const auto original = reinterpret_cast<decltype(&CGRequestScreenCaptureAccess)>(
    dlsym(RTLD_NEXT, "CGRequestScreenCaptureAccess")
  );
  return original && original();
}

TEST(MacosPermissions, ReadsGrantAndRevocationWithoutPrompting) {
  permission_answers answers;
  EXPECT_FALSE(platf::is_screen_capture_allowed());
  answers.allowed = true;
  EXPECT_TRUE(platf::is_screen_capture_allowed());
  answers.allowed = false;
  EXPECT_FALSE(platf::is_screen_capture_allowed());
  EXPECT_EQ(answers.requests, 0);
}

TEST(MacosPermissions, ExistingGrantDoesNotRequestAgain) {
  permission_answers answers;
  answers.allowed = true;
  EXPECT_NE(platf::init(), nullptr);
  EXPECT_EQ(answers.requests, 0);
}

TEST(MacosPermissions, RechecksAccessAfterGrantDuringRequest) {
  permission_answers answers;
  answers.allow_on_request = true;
  answers.request_result = true;
  EXPECT_NE(platf::init(), nullptr);
  EXPECT_EQ(answers.requests, 1);
  EXPECT_TRUE(platf::is_screen_capture_allowed());
}

TEST(MacosPermissions, RequestSuccessAloneDoesNotAuthorizeCapture) {
  permission_answers answers;
  answers.request_result = true;
  EXPECT_EQ(platf::init(), nullptr);
  EXPECT_EQ(answers.requests, 1);
  EXPECT_FALSE(platf::is_screen_capture_allowed());
}

TEST(MacosPermissions, FailedInitializationDoesNotLatchDenial) {
  permission_answers answers;
  EXPECT_EQ(platf::init(), nullptr);
  EXPECT_EQ(answers.requests, 1);
  answers.allowed = true;
  EXPECT_TRUE(platf::is_screen_capture_allowed());
  EXPECT_EQ(answers.requests, 1);
}

#endif
