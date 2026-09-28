/** @file Admission for explicit virtual-display mirroring. */
#pragma once

#include <stdbool.h>
#include <string.h>

static inline bool vd_model_is_virtual_mac(const char *model) {
  return model && strncmp(model, "VirtualMac", strlen("VirtualMac")) == 0;
}

#ifdef __APPLE__
#include <stdio.h>
#include <sys/sysctl.h>

static inline bool vd_mirror_request_allowed(const char *layout) {
  if (!layout || strcmp(layout, "mirror") != 0) return true;
  char model[128] = {0};
  size_t size = sizeof(model) - 1;
  if (sysctlbyname("hw.model", model, &size, NULL, 0) != 0 || !model[0]) {
    fprintf(stderr, "[virtual_display] Mirror request rejected: unable to read hardware model\n");
    return false;
  }
  if (!vd_model_is_virtual_mac(model)) return true;
  // Native mirror transactions on VirtualMac have destroyed the online display
  // topology and terminated the graphical session. WindowServer's cause is unknown.
  fprintf(stderr, "[virtual_display] Mirror request rejected on VirtualMac before display changes\n");
  return false;
}
#endif
