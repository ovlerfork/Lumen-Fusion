/**
 * @file tests/unit/test_http_log.cpp
 * @brief Diagnostic requests must not expose session or authentication secrets.
 */
#include <gtest/gtest.h>
#include "src/http_log.h"

TEST(HttpLog, SensitiveFieldsAreCaseInsensitive) {
  for (const auto name : {"Authorization", "authorization", "AUTHORIZATION", "Proxy-Authorization",
                          "Cookie", "Set-Cookie", "rikey", "RiKeY", "pin", "PASSWORD", "token",
                          "csrf_token", "X-CSRF-Token", "clientchallenge", "serverchallengeresp",
                          "clientpairingsecret", "serverpairingsecret"}) {
    EXPECT_EQ(http::diagnostic_value(name, "test-secret"), "CREDENTIALS REDACTED") << name;
  }
}

TEST(HttpLog, NonsecretDiagnosticsArePreserved) {
  for (const auto name : {"mode", "User-Agent", "surroundAudioInfo", "clientRefreshRateX100", "rikeyid", ""}) {
    EXPECT_EQ(http::diagnostic_value(name, "1600x1112x60"), "1600x1112x60") << name;
  }
  EXPECT_EQ(http::diagnostic_value("mode", ""), "");
}
