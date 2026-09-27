/**
 * @file src/platform/macos/vd_helper.m
 * @brief Helper process to create and hold a CGVirtualDisplay.
 *
 * Spawned by Sunshine to create virtual displays in a clean process context.
 * Usage: vd_helper <width> <height> <fps> [layout] [--managed]
 *   layout: "extend" (default), "primary", "mirror", or "system"
 * Outputs: displayID on stdout (or "0" on failure)
 * Managed stdin accepts "layout local_main_id\n" and replies with the ID or 0.
 * EOF or a termination signal releases the display after layout restoration.
 *
 * CGVirtualDisplay creates the display object, then we:
 *   1. SLSConfigureDisplayEnabled activates it in WindowServer's display list
 *   2. Apply the requested layout:
 *      - extend: CGConfigureDisplayMirrorOfDisplay(kCGNullDirectDisplay) forces
 *        extend mode (macOS may auto-mirror new displays, hiding them from
 *        CGGetActiveDisplayList)
 *      - mirror: mirror the main display
 *      - system: leave the mirror state alone, so WindowServer's persisted
 *        arrangement for this display identity (set in System Settings) applies
 *
 * The descriptor's vendor/product/serial triple is fixed, not randomized:
 * WindowServer keys its persisted per-display configuration (arrangement,
 * mirror set, resolution) off that triple. A random serial would make macOS
 * treat every session as a brand-new monitor and discard the saved layout.
 *
 * Compiled with ARC (-fobjc-arc).
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Private CGVirtualDisplay API interface declarations (macOS 14+)
@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (nonatomic) unsigned int hiDPI;
@property (retain, nonatomic) NSArray *modes;
@end

@interface CGVirtualDisplayDescriptor : NSObject
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int vendorID;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) CGPoint whitePoint;
@property (nonatomic) CGPoint redPrimary;
@property (nonatomic) CGPoint greenPrimary;
@property (nonatomic) CGPoint bluePrimary;
@property (retain, nonatomic) dispatch_queue_t queue;
@property (copy, nonatomic) void (^terminationHandler)(id, id);
- (void)setDispatchQueue:(dispatch_queue_t)queue;
@end

@interface CGVirtualDisplay : NSObject
@property (readonly, nonatomic) unsigned int displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

// SkyLight private C functions for display configuration (linked directly)
extern CGError SLSBeginDisplayConfiguration(CGDisplayConfigRef *);
extern CGError SLSConfigureDisplayEnabled(CGDisplayConfigRef, CGDirectDisplayID, bool);
extern CGError SLSConfigureDisplayOrigin(CGDisplayConfigRef, CGDirectDisplayID, int32_t, int32_t);
extern CGError SLSCompleteDisplayConfiguration(CGDisplayConfigRef, CGConfigureOption, uint32_t);

// Static storage to keep objects alive (ARC retains static references)
static CGVirtualDisplay *keepAlive = nil;
static CGVirtualDisplayDescriptor *keepDesc = nil;

static volatile sig_atomic_t shouldExit = 0;

// Requested display layout, from argv[4].
typedef enum {
  VD_LAYOUT_EXTEND = 0,  // force extend (un-mirror)
  VD_LAYOUT_MIRROR,      // force mirroring of the main display
  VD_LAYOUT_SYSTEM,      // leave whatever WindowServer restored
  VD_LAYOUT_PRIMARY
} vd_layout_t;

// Fixed EDID identity. WindowServer persists per-display settings (arrangement,
// mirror set, resolution) keyed off vendor/product/serial, so these must be
// stable across runs for the user's System Settings choice to survive a reconnect.
static const unsigned int kVendorID = 0xF0F0;
static const unsigned int kProductID = 0x5678;
static const unsigned int kSerialNum = 0x53554E31;  // 'SUN1'

static void handle_signal(int sig) {
  (void)sig;
  shouldExit = 1;
}

static BOOL checkDisplayInList(uint32_t targetID, uint32_t *outCount) {
  CGDirectDisplayID activeDisplays[32];
  uint32_t displayCount = 0;
  if (CGGetActiveDisplayList(32, activeDisplays, &displayCount) == kCGErrorSuccess) {
    if (outCount) *outCount = displayCount;
    for (uint32_t i = 0; i < displayCount; i++) {
      if (activeDisplays[i] == targetID) return YES;
    }
  }
  return NO;
}

// Snapshot before creating the VD, because registration can change the main display.
static NSMutableDictionary<NSNumber *, NSValue *> *originalOrigins;
static NSMutableDictionary<NSNumber *, NSNumber *> *originalMirrors;
static CGDirectDisplayID originalMain;
static BOOL layoutChanged;
// Current local origins = saved original origins + appliedTranslation.
static NSPoint appliedTranslation;

static BOOL rememberDisplays(void) {
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  if (CGGetOnlineDisplayList(64, displays, &count) != kCGErrorSuccess || count == 64)
    return NO;
  for (uint32_t i = 0; i < count; ++i) {
    CGDirectDisplayID id = displays[i];
    if (id == keepAlive.displayID || originalOrigins[@(id)]) continue;
    NSPoint origin = NSPointFromCGPoint(CGDisplayBounds(id).origin);
    origin.x -= appliedTranslation.x;
    origin.y -= appliedTranslation.y;
    originalOrigins[@(id)] = [NSValue valueWithPoint:origin];
    originalMirrors[@(id)] = @(CGDisplayMirrorsDisplay(id));
  }
  return YES;
}

static CGDirectDisplayID localMain(CGDirectDisplayID virtualID, CGDirectDisplayID preferred) {
  if (preferred && preferred != virtualID && CGDisplayIsOnline(preferred) && CGDisplayIsActive(preferred))
    return preferred;
  if (originalMain && originalMain != virtualID && CGDisplayIsOnline(originalMain) && CGDisplayIsActive(originalMain))
    return originalMain;
  for (NSNumber *key in originalOrigins) {
    CGDirectDisplayID id = key.unsignedIntValue;
    if (id != virtualID && CGDisplayIsOnline(id) && CGDisplayIsActive(id)) return id;
  }
  return 0;
}

static void restoreLayout(void) {
  if (!layoutChanged) return;
  CGDisplayConfigRef config = NULL;
  if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) return;
  CGError err = kCGErrorSuccess;
  if (keepAlive && CGDisplayIsOnline(keepAlive.displayID) &&
      CGConfigureDisplayMirrorOfDisplay(config, keepAlive.displayID, kCGNullDirectDisplay) != kCGErrorSuccess)
    err = kCGErrorFailure;
  for (NSNumber *key in originalOrigins) {
    CGDirectDisplayID id = key.unsignedIntValue;
    if (!CGDisplayIsOnline(id)) continue;
    NSPoint origin = originalOrigins[key].pointValue;
    CGDirectDisplayID mirror = originalMirrors[key].unsignedIntValue;
    if (mirror && !CGDisplayIsOnline(mirror)) mirror = 0;
    if (CGConfigureDisplayMirrorOfDisplay(config, id, mirror) != kCGErrorSuccess) err = kCGErrorFailure;
    if (CGConfigureDisplayOrigin(config, id, (int32_t)origin.x, (int32_t)origin.y) != kCGErrorSuccess) err = kCGErrorFailure;
  }
  if (keepAlive && CGDisplayIsOnline(keepAlive.displayID)) {
    // Vacate (0,0) before restoring the original main while the VD still exists.
    CGRect bounds = CGDisplayBounds(originalMain);
    if (CGConfigureDisplayOrigin(config, keepAlive.displayID, (int32_t)CGRectGetMaxX(bounds), 0) != kCGErrorSuccess)
      err = kCGErrorFailure;
  }
  if (originalMain && CGDisplayIsOnline(originalMain) &&
      CGConfigureDisplayOrigin(config, originalMain, 0, 0) != kCGErrorSuccess) err = kCGErrorFailure;
  if (err == kCGErrorSuccess) err = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
  else CGCancelDisplayConfiguration(config);
  if (err != kCGErrorSuccess) fprintf(stderr, "[vd_helper] Could not restore display layout: %d\n", err);
}

static BOOL applyLayout(CGDirectDisplayID virtualID, vd_layout_t layout, CGDirectDisplayID preferred) {
  if (layout == VD_LAYOUT_SYSTEM) return YES;
  if (!rememberDisplays()) return NO;
  CGDirectDisplayID main = localMain(virtualID, preferred);
  if (layout == VD_LAYOUT_MIRROR && !main) return YES; // No local master in a headless session.
  CGDisplayConfigRef config = NULL;
  if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) return NO;
  CGError err = CGConfigureDisplayMirrorOfDisplay(config, virtualID,
      layout == VD_LAYOUT_MIRROR ? main : kCGNullDirectDisplay);
  NSPoint translation = appliedTranslation;
  {
    // Unmirror local displays that WindowServer attached to our VD.
    for (NSNumber *key in originalOrigins) {
      CGDirectDisplayID id = key.unsignedIntValue;
      if (CGDisplayIsOnline(id) && CGDisplayMirrorsDisplay(id) == virtualID &&
          CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay) != kCGErrorSuccess)
        err = kCGErrorFailure;
    }
    NSPoint anchor = main ? originalOrigins[@(main)].pointValue : NSZeroPoint;
    CGFloat left = anchor.x;
    for (NSNumber *key in originalOrigins) {
      if (CGDisplayIsOnline(key.unsignedIntValue))
        left = MIN(left, originalOrigins[key].pointValue.x);
    }
    CGFloat offset = layout == VD_LAYOUT_PRIMARY ? CGDisplayBounds(virtualID).size.width + anchor.x - left : 0;
    translation = NSMakePoint(offset - anchor.x, -anchor.y);
    CGFloat right = 0;
    for (NSNumber *key in originalOrigins) {
      CGDirectDisplayID id = key.unsignedIntValue;
      if (!CGDisplayIsOnline(id)) continue;
      NSPoint origin = originalOrigins[key].pointValue;
      origin.x += translation.x;
      origin.y += translation.y;
      right = MAX(right, origin.x + CGDisplayBounds(id).size.width);
      if (CGConfigureDisplayOrigin(config, id, (int32_t)origin.x,
                                   (int32_t)origin.y) != kCGErrorSuccess) err = kCGErrorFailure;
    }
    if (layout != VD_LAYOUT_MIRROR &&
        CGConfigureDisplayOrigin(config, virtualID, layout == VD_LAYOUT_PRIMARY ? 0 : (int32_t)right, 0) != kCGErrorSuccess)
      err = kCGErrorFailure;
    if (layout != VD_LAYOUT_PRIMARY && main && CGConfigureDisplayOrigin(config, main, 0, 0) != kCGErrorSuccess)
      err = kCGErrorFailure;
  }
  if (err != kCGErrorSuccess) { CGCancelDisplayConfiguration(config); return NO; }
  layoutChanged = YES;
  if (CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly) != kCGErrorSuccess) return NO;
  appliedTranslation = translation;
  return YES;
}

static vd_layout_t parseLayout(const char *s) {
  if (!s) return VD_LAYOUT_EXTEND;
  if (strcmp(s, "primary") == 0) return VD_LAYOUT_PRIMARY;
  if (strcmp(s, "mirror") == 0) return VD_LAYOUT_MIRROR;
  if (strcmp(s, "system") == 0) return VD_LAYOUT_SYSTEM;
  return VD_LAYOUT_EXTEND;
}

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    if (argc != 4 && argc != 5 && !(argc == 6 && strcmp(argv[5], "--managed") == 0)) {
      fprintf(stdout, "0\n");
      fflush(stdout);
      return 1;
    }

    int width = atoi(argv[1]);
    int height = atoi(argv[2]);
    int fps = atoi(argv[3]);
    vd_layout_t layout = parseLayout(argc >= 5 ? argv[4] : NULL);

    if (width <= 0 || height <= 0 || fps <= 0) {
      fprintf(stdout, "0\n");
      fflush(stdout);
      return 1;
    }

    // Runtime availability check
    if (!NSClassFromString(@"CGVirtualDisplay")) {
      fprintf(stderr, "[vd_helper] CGVirtualDisplay API not available\n");
      fprintf(stdout, "0\n");
      fflush(stdout);
      return 1;
    }

    // Initialize NSApplication
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];

    // Set up signal handlers
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, handle_signal);

    // SIGPIPE must not bypass restoration when the parent exits mid-response.
    signal(SIGPIPE, SIG_IGN);
    BOOL managed = argc == 6;
    if (managed && fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0) {
      printf("0\n");
      fflush(stdout);
      return 1;
    }
    originalOrigins = [NSMutableDictionary dictionary];
    originalMirrors = [NSMutableDictionary dictionary];
    originalMain = CGMainDisplayID();
    if (!rememberDisplays()) { printf("0\n"); fflush(stdout); return 1; }

    // Create display directly on main thread
    CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
    desc.name = @"Sunshine Virtual Display";
    desc.vendorID = kVendorID;
    desc.productID = kProductID;
    desc.serialNum = kSerialNum;
    desc.maxPixelsWide = (unsigned int)width;
    desc.maxPixelsHigh = (unsigned int)height;
    // Fixed 27" monitor physical size — do NOT scale linearly with resolution.
    // WindowServer rejects displays with unreasonably large physical dimensions.
    desc.sizeInMillimeters = CGSizeMake(597, 336);
    desc.whitePoint = CGPointMake(0.3127, 0.3290);
    desc.redPrimary = CGPointMake(0.64, 0.33);
    desc.greenPrimary = CGPointMake(0.30, 0.60);
    desc.bluePrimary = CGPointMake(0.15, 0.06);
    [desc setDispatchQueue:dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0)];
    desc.terminationHandler = ^(id s, id d) {
      fprintf(stderr, "[vd_helper] Virtual display terminated by system\n");
    };

    CGVirtualDisplayMode *nativeMode = [[CGVirtualDisplayMode alloc] initWithWidth:(unsigned int)width
                                                                          height:(unsigned int)height
                                                                     refreshRate:(double)fps];
    if (!nativeMode) {
      fprintf(stderr, "[vd_helper] Failed to create CGVirtualDisplayMode\n");
      fprintf(stdout, "0\n");
      fflush(stdout);
      return 1;
    }

    // Build mode list with native + half-resolution mode.
    // With hiDPI=1, macOS selects the native mode as the retina backing store
    // and the half-res mode as the logical resolution (2x scaling).
    // Without this, macOS only gives us half the requested pixel resolution.
    CGVirtualDisplayMode *halfMode = [[CGVirtualDisplayMode alloc] initWithWidth:(unsigned int)(width / 2)
                                                                         height:(unsigned int)(height / 2)
                                                                    refreshRate:(double)fps];
    CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
    settings.hiDPI = 1;
    if (halfMode) {
      settings.modes = @[nativeMode, halfMode];
    } else {
      settings.modes = @[nativeMode];
    }

    BOOL settingsApplied = NO;
    CGVirtualDisplay *display = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
    if (!display) {
      fprintf(stderr, "[vd_helper] initWithDescriptor returned nil (trying background thread)\n");

      // Fallback: try on background thread
      __block CGVirtualDisplay *bgDisplay = nil;
      __block BOOL bgApplied = NO;
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      dispatch_async(dispatch_get_global_queue(0, 0), ^{
        bgDisplay = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
        if (bgDisplay) bgApplied = [bgDisplay applySettings:settings];
        dispatch_semaphore_signal(sem);
      });
      if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC)) != 0) {
        fprintf(stdout, "0\n");
        fflush(stdout);
        return 1;
      }
      display = bgDisplay;
      settingsApplied = bgApplied;
    } else {
      settingsApplied = [display applySettings:settings];
    }

    keepAlive = display;
    layoutChanged = display != nil;
    if (!display || display.displayID == 0 || !settingsApplied) {
      fprintf(stderr, "[vd_helper] Failed to create virtual display\n");
      restoreLayout();
      fprintf(stdout, "0\n");
      fflush(stdout);
      return 1;
    }

    keepAlive = display;
    keepDesc = desc;
    uint32_t resultID = display.displayID;

    fprintf(stderr, "[vd_helper] Display %u created, activating...\n", resultID);

    // Step 1: Activate display via SkyLight SLSConfigureDisplayEnabled
    {
      CGDisplayConfigRef cgConfig = NULL;
      CGError err = SLSBeginDisplayConfiguration(&cgConfig);
      fprintf(stderr, "[vd_helper] SLSBeginDisplayConfiguration: %d\n", err);
      if (err == kCGErrorSuccess && cgConfig) {
        err = SLSConfigureDisplayEnabled(cgConfig, resultID, true);
        fprintf(stderr, "[vd_helper] SLSConfigureDisplayEnabled(%u, true): %d\n", resultID, err);
        CGDirectDisplayID mainDisplay = CGMainDisplayID();
        CGFloat mainWidth = CGRectGetMaxX(CGDisplayBounds(mainDisplay));
        if (layout != VD_LAYOUT_SYSTEM)
          SLSConfigureDisplayOrigin(cgConfig, resultID, (int32_t)mainWidth, 0);
        layoutChanged = YES;
        CGError completeErr = SLSCompleteDisplayConfiguration(cgConfig, kCGConfigureForAppOnly, 0);
        fprintf(stderr, "[vd_helper] SLSCompleteDisplayConfiguration: %d\n", completeErr);
      }
    }

    // Wait for WindowServer to process the display
    usleep(500000); // 500ms

    // Step 2: Apply the requested layout
    BOOL applied = applyLayout(resultID, layout, 0);

    // Step 3: Switch to native resolution (1x scale) mode.
    // The display starts as retina 2x (logical=half, pixel=full).
    // For streaming, we want native 1x (logical=full, pixel=full) to avoid
    // compositor overhead that causes latency and FPS drops.
    // Skipped while mirrored: the mirror master drives the mode, and setting a
    // mode on the slave would just tear the mirror set down.
    if (CGDisplayIsInMirrorSet(resultID)) {
      fprintf(stderr, "[vd_helper] Display %u is mirrored, leaving mode to the mirror master\n", resultID);
    } else {
      NSDictionary *opts = @{(NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES};
      CFArrayRef allModes = CGDisplayCopyAllDisplayModes(resultID, (CFDictionaryRef)opts);
      if (allModes) {
        CGDisplayModeRef nativeMode = NULL;
        CFIndex modeCount = CFArrayGetCount(allModes);
        for (CFIndex i = 0; i < modeCount; i++) {
          CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(allModes, i);
          size_t lw = CGDisplayModeGetWidth(m);
          size_t lh = CGDisplayModeGetHeight(m);
          size_t pw = CGDisplayModeGetPixelWidth(m);
          size_t ph = CGDisplayModeGetPixelHeight(m);
          // Find the 1x native mode matching our requested resolution
          if ((int)lw == width && (int)lh == height && pw == lw && ph == lh) {
            nativeMode = m;
            break;
          }
        }
        if (nativeMode) {
          CGError modeErr = CGDisplaySetDisplayMode(resultID, nativeMode, NULL);
          fprintf(stderr, "[vd_helper] Switched to native %dx%d (1x scale): %d\n", width, height, modeErr);
        } else {
          fprintf(stderr, "[vd_helper] Native %dx%d mode not found, staying at retina 2x\n", width, height);
        }
        CFRelease(allModes);
      }
    }

    // Wait for mode switch to take effect
    usleep(500000); // 500ms

    // Step 4: If still not visible, try again after a longer wait.
    // A mirror slave is legitimately absent from the active list, so only retry
    // (which un-mirrors) when we actually asked for extend mode.
    uint32_t count = 0;
    BOOL found = checkDisplayInList(resultID, &count);
    if (!found && layout == VD_LAYOUT_EXTEND) {
      fprintf(stderr, "[vd_helper] Display %u not found after first attempt, retrying...\n", resultID);
      sleep(1);
      // Check mirror state again
      fprintf(stderr, "[vd_helper] Mirror state (retry): inMirrorSet=%d, mirrorsDisplay=%u\n",
              CGDisplayIsInMirrorSet(resultID), CGDisplayMirrorsDisplay(resultID));
      applied = applyLayout(resultID, layout, 0);
      usleep(500000);
      found = checkDisplayInList(resultID, &count);
    }

    fprintf(stderr, "[vd_helper] Display %u (%dx%d@%dHz) - %s in active list (%u total)\n",
            resultID, width, height, fps, found ? "FOUND" : "NOT found", count);

    // Log all active displays for debugging
    {
      CGDirectDisplayID activeDisplays[32];
      uint32_t dCount = 0;
      CGGetActiveDisplayList(32, activeDisplays, &dCount);
      for (uint32_t i = 0; i < dCount; i++) {
        fprintf(stderr, "[vd_helper]   active[%u] = %u (online=%d, active=%d, mirror=%u)\n",
                i, activeDisplays[i],
                CGDisplayIsOnline(activeDisplays[i]),
                CGDisplayIsActive(activeDisplays[i]),
                CGDisplayMirrorsDisplay(activeDisplays[i]));
      }
      // Also check our display specifically
      fprintf(stderr, "[vd_helper]   ours[%u]: online=%d, active=%d, inMirror=%d, mirrors=%u\n",
              resultID,
              CGDisplayIsOnline(resultID),
              CGDisplayIsActive(resultID),
              CGDisplayIsInMirrorSet(resultID),
              CGDisplayMirrorsDisplay(resultID));
    }

    // Reapply after the 1x mode switch so placement uses the final logical bounds.
    if (layout != VD_LAYOUT_SYSTEM) applied = applyLayout(resultID, layout, 0) && applied;
    BOOL usable = CGDisplayIsOnline(resultID) &&
                  (CGDisplayIsActive(resultID) || CGDisplayMirrorsDisplay(resultID));
    fprintf(stdout, "%u\n", applied && usable ? resultID : 0);
    fflush(stdout);
    if (!applied || !usable) shouldExit = 1;

    char command[128];
    size_t used = 0;
    while (!shouldExit) {
      if (managed) {
        char c;
        ssize_t n;
        while ((n = read(STDIN_FILENO, &c, 1)) == 1) {
          if (c == '\n') {
            command[used] = 0;
            char requested[16], extra;
            unsigned int localID;
            BOOL valid = sscanf(command, "%15s %u %c", requested, &localID, &extra) == 2 &&
                (!strcmp(requested, "extend") || !strcmp(requested, "primary") ||
                 !strcmp(requested, "mirror") || !strcmp(requested, "system"));
            BOOL ok = valid && CGDisplayIsOnline(resultID) && applyLayout(resultID, parseLayout(requested), localID);
            fprintf(stdout, "%u\n", ok ? resultID : 0);
            fflush(stdout);
            used = 0;
          } else if (used < sizeof(command) - 1) command[used++] = c;
          else { shouldExit = 1; break; }
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) shouldExit = 1;
      }
      if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false) == kCFRunLoopRunFinished)
        usleep(100000);
    }

    fprintf(stderr, "[vd_helper] Shutting down, releasing display %u\n", resultID);
    restoreLayout();
    keepAlive = nil;
    keepDesc = nil;
  }
  return 0;
}
