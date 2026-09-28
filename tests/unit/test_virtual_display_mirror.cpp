/** @file VirtualMac mirror admission model identification. */
#include <gtest/gtest.h>

#ifdef __APPLE__
#include <cstring>
#include <sys/sysctl.h>

namespace {
  const char *hardware_model = nullptr;

  int read_hardware_model(const char *name, void *value, size_t *size, void *, size_t) {
    if (std::strcmp(name, "hw.model") || !hardware_model || *size < std::strlen(hardware_model) + 1) return -1;
    *size = std::strlen(hardware_model) + 1;
    std::memcpy(value, hardware_model, *size);
    return 0;
  }
}

// Substitute only the model lookup; execute the production admission function.
#define sysctlbyname read_hardware_model
#endif
#include "src/platform/macos/virtual_display_mirror.h"
#ifdef __APPLE__
#undef sysctlbyname
#endif

TEST(VirtualDisplayMirrorAdmission, IdentifiesVirtualMacModelFamily) {
  EXPECT_TRUE(vd_model_is_virtual_mac("VirtualMac2,1"));
  EXPECT_TRUE(vd_model_is_virtual_mac("VirtualMac1,1"));
  EXPECT_FALSE(vd_model_is_virtual_mac("Mac14,7"));
  EXPECT_FALSE(vd_model_is_virtual_mac("MacBookPro18,3"));
  EXPECT_FALSE(vd_model_is_virtual_mac(""));
  EXPECT_FALSE(vd_model_is_virtual_mac(nullptr));
}

#ifdef __APPLE__
TEST(VirtualDisplayMirrorAdmission, RejectsMirrorWithoutBlockingIndependentLayouts) {
  for (const char *model : {"VirtualMac2,1", ""}) {
    hardware_model = model;
    EXPECT_FALSE(vd_mirror_request_allowed("mirror"));
    EXPECT_FALSE(vd_mirror_request_allowed("mirror"));
    for (const char *layout : {"extend", "primary", "system", ""}) {
      EXPECT_TRUE(vd_mirror_request_allowed(layout));
    }
    EXPECT_TRUE(vd_mirror_request_allowed(nullptr));
  }
  hardware_model = nullptr;
  EXPECT_FALSE(vd_mirror_request_allowed("mirror"));
  EXPECT_TRUE(vd_mirror_request_allowed("extend"));
  hardware_model = "Mac14,7";
  EXPECT_TRUE(vd_mirror_request_allowed("mirror"));
}
#endif
