/**
 * @file tests/unit/test_virtual_display_config.cpp
 * @brief Validate virtual-display configuration values.
 */
#include "../tests_common.h"

#include <filesystem>

#include "src/config.h"

namespace config {
  void apply_config(std::unordered_map<std::string, std::string> &&vars);
}

class VirtualDisplayConfigTest: public ::testing::Test {
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

TEST_F(VirtualDisplayConfigTest, DefaultsAndInvalidValuesPreserveExistingLayout) {
  apply("");
  EXPECT_EQ(config::video.virtual_display, "disabled");
  EXPECT_EQ(config::video.virtual_display_layout, "extend");
  EXPECT_EQ(config::video.virtual_display_local_disconnect, "remove");
  EXPECT_EQ(config::video.virtual_display_headless_disconnect, "retain");
  EXPECT_EQ(config::video.virtual_display_retention_power, "display");
  EXPECT_EQ(config::video.virtual_display_local_override, "auto");

  apply("virtual_display = invalid\n"
        "virtual_display_layout = invalid\n"
        "virtual_display_local_disconnect = invalid\n"
        "virtual_display_headless_disconnect = invalid\n"
        "virtual_display_retention_power = invalid\n"
        "virtual_display_local_override = invalid\n");
  EXPECT_EQ(config::video.virtual_display, "disabled");
  EXPECT_EQ(config::video.virtual_display_layout, "extend");
  EXPECT_EQ(config::video.virtual_display_local_disconnect, "remove");
  EXPECT_EQ(config::video.virtual_display_headless_disconnect, "retain");
  EXPECT_EQ(config::video.virtual_display_retention_power, "display");
  EXPECT_EQ(config::video.virtual_display_local_override, "auto");
}

TEST_F(VirtualDisplayConfigTest, AcceptsLayoutsAndAdaptivePolicies) {
  for (const auto *layout : {"extend", "mirror", "system", "adaptive", "primary"}) {
    apply(std::string {"virtual_display_layout = "} + layout);
    EXPECT_EQ(config::video.virtual_display_layout, layout);
  }
  apply("virtual_display = enabled\n"
        "virtual_display_layout = adaptive\n"
        "virtual_display_local_disconnect = retain\n"
        "virtual_display_headless_disconnect = remove\n"
        "virtual_display_retention_power = system\n"
        "virtual_display_local_override = present\n");
  EXPECT_EQ(config::video.virtual_display, "enabled");
  EXPECT_EQ(config::video.virtual_display_layout, "adaptive");
  EXPECT_EQ(config::video.virtual_display_local_disconnect, "retain");
  EXPECT_EQ(config::video.virtual_display_headless_disconnect, "remove");
  EXPECT_EQ(config::video.virtual_display_retention_power, "system");
  EXPECT_EQ(config::video.virtual_display_local_override, "present");
  apply("virtual_display_retention_power = none\n"
        "virtual_display_local_override = absent\n");
  EXPECT_EQ(config::video.virtual_display_retention_power, "none");
  EXPECT_EQ(config::video.virtual_display_local_override, "absent");
}
