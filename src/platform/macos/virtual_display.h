/**
 * @file src/platform/macos/virtual_display.h
 * @brief Declarations for CGVirtualDisplay-based virtual display management on macOS.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Create a virtual display with the specified resolution and refresh rate.
 * @param width Display width in pixels.
 * @param height Display height in pixels.
 * @param fps Refresh rate in Hz.
 * @param layout Desired arrangement: "extend", "mirror", or "system" (leave the
 *               mirror state to WindowServer's persisted configuration).
 *               NULL is treated as "extend".
 * @return The CGDirectDisplayID of the created display, or 0 on failure.
 */
uint32_t virtual_display_create(int width, int height, int fps, const char *layout);

/**
 * @brief Destroy the currently active virtual display.
 */
void virtual_display_destroy(void);

/**
 * @brief Get the display ID of the currently active virtual display.
 * @return The CGDirectDisplayID, or 0 if no virtual display is active.
 */
uint32_t virtual_display_get_id(void);

/**
 * @brief Get the display ID that capture and input should target.
 *
 * Normally this is the virtual display itself. When the virtual display is a
 * mirror slave it is not in the active display list and cannot be captured, so
 * the mirror master (the display showing the same content) is returned instead.
 *
 * @return A capturable CGDirectDisplayID, or 0 if no virtual display is active.
 */
uint32_t virtual_display_get_target_id(void);

#ifdef __cplusplus
}
#endif
