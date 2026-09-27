/**
 * @file src/platform/macos/vd_helper.m
 * @brief Helper process to create and hold a CGVirtualDisplay.
 *
 * Spawned by Sunshine to create virtual displays in a clean process context.
 * Usage: vd_helper <width> <height> <fps> [layout] [--managed]
 *   layout: "extend" (default), "primary", "mirror", or "system"
 * Outputs: displayID on stdout (or "0" on failure)
 * Managed stdin accepts "layout local_main_id\n" and replies with the ID or 0.
 * EOF or a termination signal releases the display, then restores local layout.
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
static BOOL traceLayouts;

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
// Translation from the last successful layout; hotplug may retile locals later.
static NSPoint appliedTranslation;

static void logLayout(const char *phase) {
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  CGError err = CGGetOnlineDisplayList(64, displays, &count);
  fprintf(stderr, "[vd_helper] %s pid=%d main=%u listError=%d count=%u translation=(%.0f,%.0f)\n",
          phase, getpid(), CGMainDisplayID(), err, count, appliedTranslation.x, appliedTranslation.y);
  if (err != kCGErrorSuccess) return;
  for (uint32_t i = 0; i < count; ++i) {
    CGDirectDisplayID id = displays[i];
    CGRect bounds = CGDisplayBounds(id);
    fprintf(stderr, "[vd_helper]   display=%u active=%d mirror=%u bounds=(%.0f,%.0f %.0fx%.0f)\n",
            id, CGDisplayIsActive(id), CGDisplayMirrorsDisplay(id),
            bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
  }
}

typedef struct {
  CGDirectDisplayID online[64], active[64];
  uint32_t onlineCount, activeCount;
} DisplayLists;

static BOOL listedDisplay(const CGDirectDisplayID *ids, uint32_t count, CGDirectDisplayID id) {
  for (uint32_t i = 0; i < count; ++i) if (ids[i] == id) return YES;
  return NO;
}

static BOOL readDisplayLists(DisplayLists *lists) {
  lists->onlineCount = lists->activeCount = 0;
  CGError onlineErr = CGGetOnlineDisplayList(64, lists->online, &lists->onlineCount);
  CGError activeErr = CGGetActiveDisplayList(64, lists->active, &lists->activeCount);
  if (onlineErr != kCGErrorSuccess || activeErr != kCGErrorSuccess ||
      lists->onlineCount >= 64 || lists->activeCount >= 64) {
    fprintf(stderr, "[vd_helper] Display observation incomplete: onlineError=%d count=%u activeError=%d count=%u\n",
            onlineErr, lists->onlineCount, activeErr, lists->activeCount);
    return NO;
  }
  for (uint32_t i = 0; i < lists->activeCount; ++i) {
    if (!listedDisplay(lists->online, lists->onlineCount, lists->active[i])) {
      fprintf(stderr, "[vd_helper] Display observation changed during enumeration: active=%u missing online\n", lists->active[i]);
      return NO;
    }
  }
  return YES;
}

static void rememberDisplays(const DisplayLists *lists) {
  // Registration can insert a new display before existing locals. Recover the
  // current coordinate frame from a saved local, not our last requested offset.
  CGDirectDisplayID anchor = 0;
  for (uint32_t i = 0; i < lists->activeCount; ++i) {
    if (lists->active[i] == keepAlive.displayID || !originalOrigins[@(lists->active[i])]) continue;
    if (!anchor || lists->active[i] == originalMain) anchor = lists->active[i];
  }
  NSPoint observedTranslation = appliedTranslation;
  if (anchor) {
    CGPoint current = CGDisplayBounds(anchor).origin;
    NSPoint saved = originalOrigins[@(anchor)].pointValue;
    observedTranslation = NSMakePoint(current.x - saved.x, current.y - saved.y);
  }
  for (uint32_t i = 0; i < lists->onlineCount; ++i) {
    CGDirectDisplayID id = lists->online[i];
    if (id == keepAlive.displayID || originalOrigins[@(id)]) continue;
    NSPoint origin = NSPointFromCGPoint(CGDisplayBounds(id).origin);
    origin.x -= observedTranslation.x;
    origin.y -= observedTranslation.y;
    originalOrigins[@(id)] = [NSValue valueWithPoint:origin];
    originalMirrors[@(id)] = @(CGDisplayMirrorsDisplay(id));
    if (traceLayouts) fprintf(stderr, "[vd_helper] snapshot display=%u savedOrigin=(%.0f,%.0f) mirror=%u anchor=%u observedTranslation=(%.0f,%.0f) appliedTranslation=(%.0f,%.0f)\n",
            id, origin.x, origin.y, originalMirrors[@(id)].unsignedIntValue,
            anchor, observedTranslation.x, observedTranslation.y,
            appliedTranslation.x, appliedTranslation.y);
  }
}

static CGDirectDisplayID localMain(const DisplayLists *lists, CGDirectDisplayID virtualID, CGDirectDisplayID preferred) {
  if (preferred && preferred != virtualID && listedDisplay(lists->active, lists->activeCount, preferred))
    return preferred;
  if (originalMain && originalMain != virtualID && listedDisplay(lists->active, lists->activeCount, originalMain))
    return originalMain;
  for (uint32_t i = 0; i < lists->activeCount; ++i) {
    CGDirectDisplayID id = lists->active[i];
    if (id != virtualID) return id;
  }
  return 0;
}

static CGError configureOrigin(CGDisplayConfigRef config, CGDirectDisplayID id, int32_t x, int32_t y) {
  CGError err = CGConfigureDisplayOrigin(config, id, x, y);
  if (err != kCGErrorSuccess)
    fprintf(stderr, "[vd_helper] origin failed display=%u requested=(%d,%d) error=%d\n", id, x, y, err);
  return err;
}

static CGError configureMirror(CGDisplayConfigRef config, CGDirectDisplayID id, CGDirectDisplayID master) {
  CGError err = CGConfigureDisplayMirrorOfDisplay(config, id, master);
  if (err != kCGErrorSuccess)
    fprintf(stderr, "[vd_helper] mirror failed display=%u master=%u error=%d\n", id, master, err);
  return err;
}

static BOOL restoreLayout(void) {
  if (!layoutChanged) return YES;
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  CGError err = CGGetOnlineDisplayList(64, displays, &count);
  if (err != kCGErrorSuccess || count == 64) {
    fprintf(stderr, "[vd_helper] Cannot enumerate surviving displays: error=%d count=%u\n", err, count);
    return NO;
  }
  NSMutableSet<NSNumber *> *online = [NSMutableSet set];
  for (uint32_t i = 0; i < count; ++i) [online addObject:@(displays[i])];
  NSMutableDictionary<NSNumber *, NSNumber *> *mirrors = [NSMutableDictionary dictionary];
  CGDirectDisplayID main = 0;
  for (uint32_t i = 0; i < count; ++i) {
    CGDirectDisplayID id = displays[i];
    if (!originalOrigins[@(id)]) continue;
    CGDirectDisplayID mirror = originalMirrors[@(id)].unsignedIntValue;
    if (![online containsObject:@(mirror)]) mirror = 0;
    mirrors[@(id)] = @(mirror);
    if (!mirror && (!main || id == originalMain)) main = id;
  }
  if (!main) return YES; // No saved, independent local survives.
  NSPoint anchor = originalOrigins[@(main)].pointValue;
  fprintf(stderr, "[vd_helper] restore scope=session savedMain=%u requestedMain=%u\n", originalMain, main);
  CGDisplayConfigRef config = NULL;
  err = CGBeginDisplayConfiguration(&config);
  if (err != kCGErrorSuccess) {
    fprintf(stderr, "[vd_helper] Could not begin layout restoration: %d\n", err);
    return NO;
  }
  for (NSNumber *key in mirrors) {
    CGDirectDisplayID id = key.unsignedIntValue;
    CGDirectDisplayID mirror = mirrors[key].unsignedIntValue;
    NSPoint origin = originalOrigins[key].pointValue;
    origin.x -= anchor.x;
    origin.y -= anchor.y;
    fprintf(stderr, "[vd_helper] restore request display=%u origin=(%.0f,%.0f) mirror=%u\n",
            id, origin.x, origin.y, mirror);
    if (configureMirror(config, id, mirror) != kCGErrorSuccess) err = kCGErrorFailure;
    if (id != main && configureOrigin(config, id, (int32_t)origin.x, (int32_t)origin.y) != kCGErrorSuccess)
      err = kCGErrorFailure;
  }
  if (configureOrigin(config, main, 0, 0) != kCGErrorSuccess) err = kCGErrorFailure;
  // App-only changes roll back at helper exit. Restore just the surviving
  // locals for this login session, after our display has actually disappeared.
  if (err == kCGErrorSuccess) err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
  else {
    CGError cancelErr = CGCancelDisplayConfiguration(config);
    if (cancelErr != kCGErrorSuccess)
      fprintf(stderr, "[vd_helper] Could not cancel layout restoration: %d\n", cancelErr);
  }
  fprintf(stderr, "[vd_helper] restore scope=session completion=%d requestedMain=%u\n", err, main);
  CGDirectDisplayID active[64];
  uint32_t activeCount = 0;
  CGError listErr = CGGetActiveDisplayList(64, active, &activeCount);
  BOOL restored = err == kCGErrorSuccess && listErr == kCGErrorSuccess &&
                  activeCount > 0 && activeCount < 64 && active[0] == main;
  for (NSNumber *key in mirrors) {
    NSPoint origin = originalOrigins[key].pointValue;
    origin.x -= anchor.x;
    origin.y -= anchor.y;
    if (!CGPointEqualToPoint(CGDisplayBounds(key.unsignedIntValue).origin, NSPointToCGPoint(origin)) ||
        CGDisplayMirrorsDisplay(key.unsignedIntValue) != mirrors[key].unsignedIntValue)
      restored = NO;
  }
  logLayout("after session restore completion");
  if (!restored)
    fprintf(stderr, "[vd_helper] Session restoration not observed: requestedMain=%u listError=%d\n", main, listErr);
  return restored;
}

static BOOL releaseDisplayAndRestore(void) {
  if (!keepAlive && !layoutChanged) return YES;
  CGDirectDisplayID id = keepAlive.displayID;
  // Capture locals plugged in since the last layout command while the old
  // coordinate frame and our virtual display still exist.
  if (id && layoutChanged) {
    DisplayLists lists;
    if (readDisplayLists(&lists)) rememberDisplays(&lists);
    else fprintf(stderr, "[vd_helper] Could not snapshot locals before release\n");
  }
  logLayout("before virtual display release");
  @autoreleasepool {
    keepAlive = nil;
    keepDesc = nil;
  }
  // Stay within the parent's existing two-second graceful shutdown budget.
  // A complete online list includes mirror slaves; scalar flags can be stale.
  BOOL removed = !id;
  for (int attempt = 0; !removed && attempt < 20; ++attempt) {
    CGDirectDisplayID displays[64];
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(64, displays, &count) == kCGErrorSuccess && count < 64) {
      removed = YES;
      for (uint32_t i = 0; i < count; ++i) if (displays[i] == id) removed = NO;
    }
    if (!removed && CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false) == kCFRunLoopRunFinished)
      usleep(50000);
  }
  fprintf(stderr, "[vd_helper] release display=%u removed=%d\n", id, removed);
  logLayout("after virtual display release");
  if (!removed) {
    fprintf(stderr, "[vd_helper] Refusing session restoration while virtual display %u remains online\n", id);
    return NO;
  }
  return restoreLayout();
}

static BOOL applyLayout(CGDirectDisplayID virtualID, vd_layout_t layout, CGDirectDisplayID preferred) {
  DisplayLists lists;
  if (!readDisplayLists(&lists) || !listedDisplay(lists.online, lists.onlineCount, virtualID)) return NO;
  if (layout == VD_LAYOUT_SYSTEM) return YES;
  if (traceLayouts) logLayout("before layout");
  rememberDisplays(&lists);
  CGDirectDisplayID main = localMain(&lists, virtualID, preferred);
  const char *name = layout == VD_LAYOUT_PRIMARY ? "primary" : layout == VD_LAYOUT_MIRROR ? "mirror" : "extend";
  if (traceLayouts) fprintf(stderr, "[vd_helper] layout=%s virtual=%u preferred=%u local=%u requestedMain=%u\n",
          name, virtualID, preferred, main, layout == VD_LAYOUT_PRIMARY ? virtualID : main);
  if (layout == VD_LAYOUT_MIRROR && !main) return YES; // No local master in a headless session.
  CGDisplayConfigRef config = NULL;
  CGError beginErr = CGBeginDisplayConfiguration(&config);
  if (beginErr != kCGErrorSuccess) {
    fprintf(stderr, "[vd_helper] layout=%s begin failed: %d\n", name, beginErr);
    return NO;
  }
  if (traceLayouts) fprintf(stderr, "[vd_helper] request virtual=%u mirror=%u\n",
          virtualID, layout == VD_LAYOUT_MIRROR ? main : kCGNullDirectDisplay);
  CGError err = configureMirror(config, virtualID,
      layout == VD_LAYOUT_MIRROR ? main : kCGNullDirectDisplay);
  NSPoint translation = appliedTranslation;
  {
    // Unmirror local displays that WindowServer attached to our VD.
    for (uint32_t i = 0; i < lists.onlineCount; ++i) {
      CGDirectDisplayID id = lists.online[i];
      if (id != virtualID && CGDisplayMirrorsDisplay(id) == virtualID) {
        if (traceLayouts) fprintf(stderr, "[vd_helper] request display=%u mirror=0\n", id);
        if (configureMirror(config, id, kCGNullDirectDisplay) != kCGErrorSuccess)
          err = kCGErrorFailure;
      }
    }
    NSPoint anchor = main ? originalOrigins[@(main)].pointValue : NSZeroPoint;
    CGFloat left = anchor.x;
    for (uint32_t i = 0; i < lists.onlineCount; ++i) {
      if (lists.online[i] != virtualID)
        left = MIN(left, originalOrigins[@(lists.online[i])].pointValue.x);
    }
    CGFloat offset = layout == VD_LAYOUT_PRIMARY ? CGDisplayBounds(virtualID).size.width + anchor.x - left : 0;
    translation = NSMakePoint(offset - anchor.x, -anchor.y);
    CGFloat right = 0;
    for (uint32_t i = 0; i < lists.onlineCount; ++i) {
      CGDirectDisplayID id = lists.online[i];
      if (id == virtualID) continue;
      NSNumber *key = @(id);
      NSPoint origin = originalOrigins[key].pointValue;
      origin.x += translation.x;
      origin.y += translation.y;
      right = MAX(right, origin.x + CGDisplayBounds(id).size.width);
      if (traceLayouts) fprintf(stderr, "[vd_helper] request display=%u origin=(%.0f,%.0f) savedOrigin=(%.0f,%.0f)\n",
              id, origin.x, origin.y, originalOrigins[key].pointValue.x, originalOrigins[key].pointValue.y);
      if (configureOrigin(config, id, (int32_t)origin.x,
                          (int32_t)origin.y) != kCGErrorSuccess) err = kCGErrorFailure;
    }
    if (layout != VD_LAYOUT_MIRROR) {
      if (traceLayouts) fprintf(stderr, "[vd_helper] request virtual=%u origin=(%.0f,0)\n",
              virtualID, layout == VD_LAYOUT_PRIMARY ? 0.0 : right);
      if (configureOrigin(config, virtualID, layout == VD_LAYOUT_PRIMARY ? 0 : (int32_t)right, 0) != kCGErrorSuccess)
        err = kCGErrorFailure;
    }
    if (layout != VD_LAYOUT_PRIMARY && main) {
      if (traceLayouts) fprintf(stderr, "[vd_helper] request main=%u origin=(0,0) last\n", main);
      if (configureOrigin(config, main, 0, 0) != kCGErrorSuccess) err = kCGErrorFailure;
    }
  }
  if (err != kCGErrorSuccess) {
    fprintf(stderr, "[vd_helper] layout=%s configuration failed: %d\n", name, err);
    CGError cancelErr = CGCancelDisplayConfiguration(config);
    if (cancelErr != kCGErrorSuccess)
      fprintf(stderr, "[vd_helper] layout=%s cancel failed: %d\n", name, cancelErr);
    return NO;
  }
  err = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
  CGDirectDisplayID active[64];
  uint32_t count = 0;
  CGError listErr = CGGetActiveDisplayList(64, active, &count);
  CGDirectDisplayID requestedMain = layout == VD_LAYOUT_PRIMARY ? virtualID : main;
  CGDirectDisplayID observedMain = listErr == kCGErrorSuccess && count ? active[0] : 0;
  if (err == kCGErrorSuccess) {
    layoutChanged = YES;
    appliedTranslation = translation;
  }
  if (traceLayouts || err != kCGErrorSuccess || listErr != kCGErrorSuccess ||
      count >= 64 || (requestedMain && observedMain != requestedMain)) {
    fprintf(stderr, "[vd_helper] layout=%s scope=app-only completion=%d listError=%d requestedMain=%u observedMain=%u\n",
            name, err, listErr, requestedMain, observedMain);
    logLayout("after layout completion");
  }
  if (err != kCGErrorSuccess || listErr != kCGErrorSuccess || count >= 64) return NO;
  return YES;
}

static vd_layout_t parseLayout(const char *s) {
  if (!s) return VD_LAYOUT_EXTEND;
  if (strcmp(s, "primary") == 0) return VD_LAYOUT_PRIMARY;
  if (strcmp(s, "mirror") == 0) return VD_LAYOUT_MIRROR;
  if (strcmp(s, "system") == 0) return VD_LAYOUT_SYSTEM;
  return VD_LAYOUT_EXTEND;
}

static int runHelper(int argc, const char *argv[]) {
  @autoreleasepool {
    const char *trace = getenv("SUNSHINE_VD_TRACE_LAYOUT");
    traceLayouts = trace && !strcmp(trace, "1");
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
    DisplayLists initialLists;
    if (!readDisplayLists(&initialLists)) { printf("0\n"); fflush(stdout); return 1; }
    originalMain = initialLists.activeCount ? initialLists.active[0] : 0;
    rememberDisplays(&initialLists);

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
    if (!display || display.displayID == 0 || !settingsApplied) {
      fprintf(stderr, "[vd_helper] Failed to create virtual display\n");
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
        BOOL positioned = NO;
        if (layout != VD_LAYOUT_SYSTEM) {
          CGError originErr = SLSConfigureDisplayOrigin(cgConfig, resultID, (int32_t)mainWidth, 0);
          if (originErr == kCGErrorSuccess) positioned = YES;
          else fprintf(stderr, "[vd_helper] Initial origin failed display=%u error=%d\n", resultID, originErr);
        }
        CGError completeErr = SLSCompleteDisplayConfiguration(cgConfig, kCGConfigureForAppOnly, 0);
        if (positioned && completeErr == kCGErrorSuccess) layoutChanged = YES;
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

    // Detailed startup observations are opt-in.
    if (traceLayouts) {
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
    DisplayLists readyLists;
    BOOL complete = readDisplayLists(&readyLists);
    CGDirectDisplayID master = complete ? CGDisplayMirrorsDisplay(resultID) : 0;
    BOOL usable = complete && listedDisplay(readyLists.online, readyLists.onlineCount, resultID) &&
                  listedDisplay(readyLists.active, readyLists.activeCount, master ? master : resultID);
    CGDisplayModeRef observedMode = usable ? CGDisplayCopyDisplayMode(resultID) : NULL;
    fprintf(stderr, "[vd_helper] Ready display=%u usable=%d requestedPixels=%dx%d observedPixels=%zux%zu master=%u\n",
            resultID, usable, width, height,
            observedMode ? CGDisplayModeGetPixelWidth(observedMode) : 0,
            observedMode ? CGDisplayModeGetPixelHeight(observedMode) : 0, master);
    if (observedMode) CGDisplayModeRelease(observedMode);
    fprintf(stdout, "%u\n", applied && usable ? resultID : 0);
    fflush(stdout);
    if (!applied || !usable) shouldExit = 1;

    char command[128];
    size_t used = 0;
    while (!shouldExit) {
      BOOL handledLayout = NO;
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
            BOOL ok = valid && applyLayout(resultID, parseLayout(requested), localID);
            handledLayout = valid;
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
      if (traceLayouts && handledLayout) logLayout("after command run loop");
    }

    fprintf(stderr, "[vd_helper] Shutting down, releasing display %u\n", resultID);
  }
  return 0;
}

int main(int argc, const char *argv[]) {
  // Drain creation/command autoreleases and local strong references before
  // clearing the final owner and waiting for WindowServer to remove the VD.
  int result = runHelper(argc, argv);
  @autoreleasepool {
    if (!releaseDisplayAndRestore()) return 1;
  }
  return result;
}
