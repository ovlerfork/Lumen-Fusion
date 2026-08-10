/**
 * @file tests/unit/test_logging.cpp
 * @brief Test src/logging.*.
 */
#include "../tests_common.h"
#include "../tests_log_checker.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <src/logging.h>
#include <string>
#include <string_view>

namespace {
  std::array log_levels = {
    std::tuple("verbose", &verbose),
    std::tuple("debug", &debug),
    std::tuple("info", &info),
    std::tuple("warning", &warning),
    std::tuple("error", &error),
    std::tuple("fatal", &fatal),
#ifdef LUMINA_ENABLE_STREAM_PERF_LOGGING
    std::tuple("performance", &performance),
#endif
  };

  constexpr auto log_file = "test_sunshine.log";

  void write_log_file(const std::filesystem::path &path, std::string_view content) {
    std::ofstream output {path};
    output << content;
  }

  std::string read_log_file(const std::filesystem::path &path) {
    std::ifstream input {path};
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }
}  // namespace

class LogRotationTest: public testing::Test {
protected:
  void SetUp() override {
    std::filesystem::remove_all(test_directory);
    std::filesystem::create_directories(test_directory);
  }

  void TearDown() override {
    std::filesystem::remove_all(test_directory);
  }

  std::filesystem::path rotated_log_path(std::size_t generation) const {
    auto path = log_path;
    path += std::format(".{}", generation);
    return path;
  }

  const std::filesystem::path test_directory {std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "log_rotation_tests"};
  const std::filesystem::path log_path {test_directory / "custom.log"};
};

TEST_F(LogRotationTest, RotatesCurrentLogAndRetainsFivePreviousLogs) {
  write_log_file(log_path, "current");
  for (std::size_t generation = 1; generation <= logging::retained_log_file_count; ++generation) {
    write_log_file(rotated_log_path(generation), std::to_string(generation));
  }

  EXPECT_FALSE(logging::rotate_log_file(log_path));

  EXPECT_FALSE(std::filesystem::exists(log_path));
  EXPECT_EQ(read_log_file(rotated_log_path(1)), "current");
  EXPECT_EQ(read_log_file(rotated_log_path(2)), "1");
  EXPECT_EQ(read_log_file(rotated_log_path(3)), "2");
  EXPECT_EQ(read_log_file(rotated_log_path(4)), "3");
  EXPECT_EQ(read_log_file(rotated_log_path(5)), "4");
}

TEST_F(LogRotationTest, SupportsMissingLogGenerations) {
  write_log_file(rotated_log_path(2), "second");

  EXPECT_FALSE(logging::rotate_log_file(log_path));

  EXPECT_FALSE(std::filesystem::exists(log_path));
  EXPECT_FALSE(std::filesystem::exists(rotated_log_path(1)));
  EXPECT_FALSE(std::filesystem::exists(rotated_log_path(2)));
  EXPECT_EQ(read_log_file(rotated_log_path(3)), "second");
}

TEST_F(LogRotationTest, ReportsFilesystemErrors) {
  std::filesystem::create_directories(rotated_log_path(logging::retained_log_file_count) / "child");
  write_log_file(log_path, "current");

  EXPECT_TRUE(logging::rotate_log_file(log_path));
  EXPECT_EQ(read_log_file(log_path), "current");
}

struct LogLevelsTest: testing::TestWithParam<decltype(log_levels)::value_type> {};

INSTANTIATE_TEST_SUITE_P(
  Logging,
  LogLevelsTest,
  testing::ValuesIn(log_levels),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(LogLevelsTest, PutMessage) {
  auto [label, plogger] = GetParam();
  ASSERT_TRUE(plogger);
  auto &logger = *plogger;

  std::random_device rand_dev;
  std::mt19937_64 rand_gen(rand_dev());
  auto test_message = std::format("{}{}", rand_gen(), rand_gen());
  BOOST_LOG(logger) << test_message;

  ASSERT_TRUE(log_checker::line_contains(log_file, test_message));
}
