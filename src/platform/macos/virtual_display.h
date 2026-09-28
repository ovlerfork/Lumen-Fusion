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
 * @param layout Desired arrangement: "extend", "primary", "mirror", or "system" (leave the
 *               mirror state to WindowServer's persisted configuration).
 *               NULL is treated as "extend".
 *               VirtualMac models reject explicit mirror requests before any
 *               display changes, preserving an existing owned helper and display.
 *               Mirror admission also fails if the hardware model cannot be read.
 * @return The CGDirectDisplayID of the created display, or 0 on failure.
 */
uint32_t virtual_display_create(int width, int height, int fps, const char *layout);

/**
 * Reuse a healthy helper with the same width/height/fps and apply the layout.
 * Changed modes or a confirmed dead helper are explicitly recreated.
 * Transient visibility/waitpid failures return 0 without dropping ownership;
 * retry the same mode to resume the same display.
 * Returns the VD ID, or 0 on failure. NULL/empty layout means extend.
 * Adaptive policy must resolve to primary or extend before calling this API.
 * VirtualMac mirror rejection returns 0 and preserves ownership and pending IPC.
 */
uint32_t virtual_display_ensure(int width, int height, int fps, const char *layout);

/**
 * Temporarily change layout without replacing the helper or display ID.
 * local_main_id selects an active local or an online local mirroring our VD;
 * the latter is detached before becoming master. 0 selects the original main
 * when eligible, then another active or promotable local display.
 * Accepts extend/primary/mirror/system. System makes no arrangement changes.
 * VirtualMac rejects mirror with 0 before sending a command or changing state.
 * Returns 1 after helper acknowledgement, 0 on failure. A mirror acknowledgement
 * requires the helper to observe the requested mirror set when an eligible
 * local exists; without one, mirror leaves the arrangement unchanged. The caller's
 * display notifications may still be pending. An uncertain reply preserves
 * helper ownership; any pending reply is drained before a new command.
 * Original available display origins/main are restored after releasing the VD,
 * before the helper exits, only when this helper changed their arrangement.
 * Calls are serialized; create/ensure/destroy may block for bounded IPC/stop.
 */
int virtual_display_apply_layout(const char *layout, uint32_t local_main_id);

/**
 * @brief Destroy the currently active virtual display.
 */
void virtual_display_destroy(void);

/**
 * @brief Get the owned display ID, including during temporary invisibility.
 * @return The CGDirectDisplayID, or 0 if absent or the helper is confirmed dead.
 */
uint32_t virtual_display_get_id(void);

/**
 * @brief Get the display ID that capture and input should target.
 *
 * Normally this is the virtual display itself. When the virtual display is a
 * mirror slave, the mirror master (the display showing the same content) is
 * returned instead. Software mirroring may list both members as active;
 * hardware mirroring lists only the master as active.
 *
 * @return A capturable CGDirectDisplayID, or 0 if no virtual display is active.
 */
uint32_t virtual_display_get_target_id(void);

#ifdef __cplusplus
}
#endif
