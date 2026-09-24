/**
 * @file src/http_log.h
 * @brief Redact credentials in diagnostic HTTP header and query logging.
 */
#pragma once

#include <algorithm>
#include <string_view>

namespace http {
  inline std::string_view diagnostic_value(std::string_view name, std::string_view value) {
    constexpr std::string_view sensitive[] {
      "authorization", "proxy-authorization", "cookie", "set-cookie",
      "rikey", "pin", "password", "token", "csrf_token", "x-csrf-token",
      "clientchallenge", "serverchallengeresp", "clientpairingsecret", "serverpairingsecret",
    };
    const auto lower_ascii = [](unsigned char ch) {
      return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
    };
    for (const auto field : sensitive) {
      if (name.size() == field.size() && std::equal(name.begin(), name.end(), field.begin(),
          [&](unsigned char left, unsigned char right) { return lower_ascii(left) == right; })) {
        return "CREDENTIALS REDACTED";
      }
    }
    return value;
  }
}  // namespace http
