/**
 * @file tests/unit/test_macos_latency_config.cpp
 * @brief Validate macOS experimental latency configuration values.
 */
#include "../tests_common.h"

#include <filesystem>

#include "src/config.h"

namespace config {
  void apply_config(std::unordered_map<std::string, std::string> &&vars);
}

class MacOSLatencyConfigTest: public ::testing::Test {
protected:
  const config::video_t saved_video = config::video;
  const config::audio_t saved_audio = config::audio;
  const config::stream_t saved_stream = config::stream;
  const config::nvhttp_t saved_nvhttp = config::nvhttp;
  const config::input_t saved_input = config::input;
  const config::sunshine_t saved_sunshine = config::sunshine;
  const std::unordered_map<std::string, std::string> saved_settings = config::modified_config_settings;

  void SetUp() override {
    // Use existing paths so configuration parsing does not create files or directories.
    const auto existing_file = (std::filesystem::path {SUNSHINE_SOURCE_DIR} / "package.json").string();
    config::stream.file_apps = existing_file;
    config::nvhttp.pkey = existing_file;
    config::nvhttp.cert = existing_file;
    config::nvhttp.file_state = existing_file;
    config::sunshine.log_file = existing_file;
  }

  void TearDown() override {
    config::video = saved_video;
    config::audio = saved_audio;
    config::stream = saved_stream;
    config::nvhttp = saved_nvhttp;
    config::input = saved_input;
    config::sunshine = saved_sunshine;
    config::modified_config_settings = saved_settings;
  }

  static void apply(const std::string &text) {
    config::apply_config(config::parse_config(text));
  }
};

TEST_F(MacOSLatencyConfigTest, DefaultsAndInvalidValuesPreserveCurrentBehavior) {
  for (const auto *value : {"", "invalid", "-2", "2", "9", "4.5", "4junk", "4294967300"}) {
    apply(std::string {"vt_low_latency_rate_control = "} + value + "\n" +
          "vt_prio_speed = " + value + "\n" +
          "vt_power_efficient = " + value + "\n" +
          "macos_capture_queue_depth = " + value + "\n");
    EXPECT_EQ(config::video.vt.vt_low_latency_rate_control, "inherit");
    EXPECT_EQ(config::video.vt.vt_prio_speed, -1);
    EXPECT_EQ(config::video.vt.vt_power_efficient, -1);
    EXPECT_EQ(config::video.macos_capture_queue_depth, 4);
  }
}

TEST_F(MacOSLatencyConfigTest, AcceptsAndRestoresExperimentalPreferences) {
  apply("vt_low_latency_rate_control = auto\nvt_prio_speed = disabled\nvt_power_efficient = enabled\n");
  EXPECT_EQ(config::video.vt.vt_low_latency_rate_control, "auto");
  EXPECT_EQ(config::video.vt.vt_prio_speed, 0);
  EXPECT_EQ(config::video.vt.vt_power_efficient, 1);
  apply("vt_prio_speed = enabled\nvt_power_efficient = disabled\n");
  EXPECT_EQ(config::video.vt.vt_prio_speed, 1);
  EXPECT_EQ(config::video.vt.vt_power_efficient, 0);
  apply("vt_low_latency_rate_control = inherit\nvt_prio_speed = inherit\nvt_power_efficient = inherit\n");
  EXPECT_EQ(config::video.vt.vt_low_latency_rate_control, "inherit");
  EXPECT_EQ(config::video.vt.vt_prio_speed, -1);
  EXPECT_EQ(config::video.vt.vt_power_efficient, -1);
  for (int depth = 3; depth <= 8; ++depth) {
    apply("macos_capture_queue_depth = " + std::to_string(depth));
    EXPECT_EQ(config::video.macos_capture_queue_depth, depth);
  }
}

TEST_F(MacOSLatencyConfigTest, ParsesH264EntropyChoicesAndKeepsTheUnsetDefault) {
  config::video.vt.vt_coder = -1;
  apply("");
  EXPECT_EQ(config::video.vt.vt_coder, -1);
  apply("vt_coder = cabac\n");
  EXPECT_EQ(config::video.vt.vt_coder, 1);
  apply("vt_coder = cavlc\n");
  EXPECT_EQ(config::video.vt.vt_coder, 2);
  apply("vt_coder = auto\n");
  EXPECT_EQ(config::video.vt.vt_coder, 0);
}
