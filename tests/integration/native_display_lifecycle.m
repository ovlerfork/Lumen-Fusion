// Run only in a disposable logged-in macOS 14+ WindowServer session.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "virtual_display.h"

#ifdef NATIVE_WRAPPER_OBJECT
// Substitute after system declarations so Darwin symbol aliases stay intact.
pid_t native_test_waitpid(pid_t, int *, int);
boolean_t native_test_display_online(CGDirectDisplayID);
boolean_t native_test_display_active(CGDirectDisplayID);
#define waitpid native_test_waitpid
#define CGDisplayIsOnline native_test_display_online
#define CGDisplayIsActive native_test_display_active
#include "virtual_display.m"
#else
extern char **environ;

// All unselected observations and helper execution remain native.
static int interruptedWait;
static pid_t observedHelper;
static CGDirectDisplayID unavailableID;
static BOOL offlineObservation, inactiveObservation;
pid_t native_test_waitpid(pid_t pid, int *status, int options) {
  if (pid > 0) observedHelper = pid;
  if (interruptedWait) { --interruptedWait; errno = EINTR; return -1; }
  return waitpid(pid, status, options);
}
boolean_t native_test_display_online(CGDirectDisplayID id) {
  return id == unavailableID && offlineObservation ? false : CGDisplayIsOnline(id);
}
boolean_t native_test_display_active(CGDirectDisplayID id) {
  return id == unavailableID && inactiveObservation ? false : CGDisplayIsActive(id);
}

// A distinct native display exercises hotplug and right-side multi-monitor layout.
@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)rate;
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
- (void)setDispatchQueue:(dispatch_queue_t)queue;
@end
@interface CGVirtualDisplay : NSObject
@property (readonly, nonatomic) unsigned int displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end
static CGVirtualDisplay *localFixture;
static pid_t crashGroup;
static BOOL crashSignalsSent;
static CGDirectDisplayID crashDisplay;

static BOOL waitForRemoval(CGDirectDisplayID id);
static BOOL cleanupCrash(void) {
  if (!crashGroup) return YES;
  // Keep the group leader unreaped until all group signals have been sent.
  // Its PID pins the group identity, including when it is already a zombie.
  if (!crashSignalsSent) {
    kill(-crashGroup, SIGTERM);
    usleep(200000);
    kill(-crashGroup, SIGKILL);
    crashSignalsSent = YES;
  }
  BOOL reaped = NO;
  for (int i = 0; i < 200; ++i) {
    pid_t result = waitpid(crashGroup, NULL, WNOHANG);
    if (result == crashGroup || (result < 0 && errno == ECHILD)) { reaped = YES; break; }
    if (result < 0 && errno != EINTR) break;
    usleep(10000);
  }
  BOOL removed = !crashDisplay || waitForRemoval(crashDisplay);
  BOOL groupGone = NO;
  for (int i = 0; i < 200; ++i) {
    if (kill(-crashGroup, 0) < 0 && errno == ESRCH) { groupGone = YES; break; }
    usleep(10000);
  }
  if (reaped) crashGroup = 0; // Never signal this group ID again after reaping.
  if (!reaped || !removed || !groupGone)
    fprintf(stderr, "Crash cleanup failed: reaped=%d removed=%d groupGone=%d\n", reaped, removed, groupGone);
  return reaped && removed && groupGone;
}

static void require(BOOL condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    interruptedWait = 0;
    offlineObservation = inactiveObservation = NO;
    cleanupCrash();
    virtual_display_destroy();
    localFixture = nil;
    exit(1);
  }
}

static BOOL waitForMain(CGDirectDisplayID id) {
  for (int i = 0; i < 100; ++i) {
    if (CGMainDisplayID() == id) return YES;
    usleep(100000);
  }
  return NO;
}

static BOOL waitForRemoval(CGDirectDisplayID id) {
  for (int i = 0; i < 100; ++i) {
    if (!CGDisplayIsOnline(id)) return YES;
    usleep(100000);
  }
  return NO;
}

static CGDirectDisplayID addLocalDisplay(CGPoint origin) {
  CGVirtualDisplayDescriptor *descriptor = [[CGVirtualDisplayDescriptor alloc] init];
  descriptor.name = @"Native lifecycle local fixture";
  descriptor.vendorID = 0xF0F0;
  descriptor.productID = 0x5679;
  descriptor.serialNum = (unsigned int)getpid();
  // Wider than the 1280px VD so parking at the translated main edge also
  // intersects this local display when restoring from primary.
  descriptor.maxPixelsWide = 1600;
  descriptor.maxPixelsHigh = 600;
  descriptor.sizeInMillimeters = CGSizeMake(300, 225);
  descriptor.whitePoint = CGPointMake(0.3127, 0.3290);
  descriptor.redPrimary = CGPointMake(0.64, 0.33);
  descriptor.greenPrimary = CGPointMake(0.30, 0.60);
  descriptor.bluePrimary = CGPointMake(0.15, 0.06);
  [descriptor setDispatchQueue:dispatch_get_main_queue()];
  CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
  settings.hiDPI = 0;
  settings.modes = @[[[CGVirtualDisplayMode alloc] initWithWidth:1600 height:600 refreshRate:60]];
  localFixture = [[CGVirtualDisplay alloc] initWithDescriptor:descriptor];
  require(localFixture && [localFixture applySettings:settings], "local native fixture creation");
  CGDirectDisplayID id = localFixture.displayID;
  BOOL online = NO;
  for (int i = 0; i < 100; ++i) {
    if (CGDisplayIsOnline(id)) { online = YES; break; }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    usleep(50000);
  }
  require(online, "local fixture online");
  CGDisplayConfigRef config;
  require(CGBeginDisplayConfiguration(&config) == kCGErrorSuccess, "fixture arrangement begin");
  CGError err = CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay);
  if (err == kCGErrorSuccess) err = CGConfigureDisplayOrigin(config, id, (int32_t)origin.x, (int32_t)origin.y);
  if (err == kCGErrorSuccess) err = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
  else CGCancelDisplayConfiguration(config);
  require(err == kCGErrorSuccess, "fixture positioned to the right");
  return id;
}

static void crashCase(char *executable, BOOL stoppedHelper) {
  int output[2];
  require(pipe(output) == 0, "crash test pipe");
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, output[0]);
  posix_spawn_file_actions_addclose(&actions, output[1]);
  posix_spawnattr_t attr;
  require(posix_spawnattr_init(&attr) == 0, "crash spawn attributes");
  require(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) == 0 &&
          posix_spawnattr_setpgroup(&attr, 0) == 0, "own crash process group");
  char *childArgv[] = {executable, "--crash-parent", NULL};
  pid_t parent;
  int err = posix_spawn(&parent, executable, &actions, &attr, childArgv, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  close(output[1]);
  require(err == 0, "crash test parent spawn");
  crashGroup = parent;
  crashSignalsSent = NO;
  crashDisplay = 0;
  struct pollfd fd = {output[0], POLLIN, 0};
  int ready = poll(&fd, 1, 15000);
  char response[32] = {0};
  ssize_t length = ready > 0 ? read(output[0], response, sizeof(response) - 1) : -1;
  close(output[0]);
  crashDisplay = length > 0 ? (uint32_t)strtoul(response, NULL, 10) : 0;
  require(crashDisplay != 0, "crash parent created VD");
  if (stoppedHelper) require(kill(-crashGroup, SIGSTOP) == 0, "stop owned crash subtree");
  require(kill(parent, SIGKILL) == 0, "crash owned parent");
  BOOL released = stoppedHelper || waitForRemoval(crashDisplay);
  BOOL cleaned = cleanupCrash();
  require(cleaned, "bounded crash subtree cleanup and display removal");
  require(released, "parent SIGKILL releases VD through EOF");
}

int main(int argc, char **argv) {
  @autoreleasepool {
    const char *githubActions = getenv("GITHUB_ACTIONS");
    require(githubActions && !strcmp(githubActions, "true"),
            "test requires a disposable GitHub Actions macOS session");
    require([[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:(NSOperatingSystemVersion){14, 0, 0}],
            "test requires macOS 14 or newer");
    signal(SIGCHLD, SIG_DFL);
    if (argc == 2 && !strcmp(argv[1], "--crash-parent")) {
      uint32_t id = virtual_display_ensure(1280, 720, 60, "primary");
      printf("%u\n", id);
      fflush(stdout);
      for (int i = 0; i < 300; ++i) usleep(100000);
      virtual_display_destroy();
      return 2;
    }
    require(argc == 2 && !strcmp(argv[1], "--disposable-windowserver"),
            "pass --disposable-windowserver to authorize temporary display changes");
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    CGDirectDisplayID originalMain = CGMainDisplayID();
    CGDirectDisplayID displays[64];
    CGRect bounds[64];
    uint32_t count = 0;
    require(CGGetActiveDisplayList(64, displays, &count) == kCGErrorSuccess && count > 0 && count < 64,
            "test needs a usable original display");
    for (uint32_t i = 0; i < count; ++i) bounds[i] = CGDisplayBounds(displays[i]);

    uint32_t id = virtual_display_ensure(1280, 720, 60, "primary");
    require(id != 0, "native VD creation");
    require(waitForMain(id), "VD becomes primary");
    require(virtual_display_ensure(1280, 720, 60, "primary") == id, "same-mode Resume retains ID");
    unavailableID = id;
    pid_t originalHelper = observedHelper;
    for (int fault = 0; fault < 3; ++fault) {
      interruptedWait = fault == 0;
      offlineObservation = fault == 1;
      inactiveObservation = fault == 2;
      require(virtual_display_ensure(1280, 720, 60, "primary") == 0, "transient observation is retryable");
      interruptedWait = fault == 0;
      require(virtual_display_get_id() == id, "transient observation preserves ownership");
      interruptedWait = fault == 0;
      require(virtual_display_get_target_id() == 0, "transient capture target is unavailable");
      offlineObservation = inactiveObservation = NO;
      require(virtual_display_ensure(1280, 720, 60, "primary") == id && observedHelper == originalHelper, "retry resumes same helper and display");
    }
    CGPoint translation = CGDisplayBounds(originalMain).origin;
    CGFloat right = 0;
    for (uint32_t i = 0; i < count; ++i) right = MAX(right, CGRectGetMaxX(CGDisplayBounds(displays[i])));
    CGPoint pluggedOrigin = CGPointMake(right, translation.y);
    CGDirectDisplayID plugged = addLocalDisplay(pluggedOrigin);
    CGPoint restoredPlugged = CGPointMake(pluggedOrigin.x - translation.x, pluggedOrigin.y - translation.y);
    require(virtual_display_apply_layout("primary", 0), "primary observes hotplug");
    CGRect pluggedPrimary = CGDisplayBounds(plugged);
    require(virtual_display_apply_layout("primary", 0), "repeated primary");
    require(CGPointEqualToPoint(CGDisplayBounds(plugged).origin, pluggedPrimary.origin), "hotplug primary is idempotent");
    require(virtual_display_apply_layout("extend", originalMain), "extension acknowledgement");
    require(virtual_display_get_id() == id && waitForMain(originalMain), "extension keeps VD and restores local main");
    require(CGPointEqualToPoint(CGDisplayBounds(plugged).origin, restoredPlugged), "hotplug normalized into original frame");
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(plugged)), "extension clears right-side local display");
    for (uint32_t i = 0; i < count; ++i) {
      require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(displays[i])), "extension clears every local display");
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "extension preserves local arrangement");
    }
    require(virtual_display_apply_layout("extend", originalMain), "repeated extension");
    require(CGPointEqualToPoint(CGDisplayBounds(plugged).origin, restoredPlugged), "extension is idempotent");
    require(virtual_display_apply_layout("primary", 0) && waitForMain(id), "primary switch keeps helper");
    require(virtual_display_get_id() == id, "primary keeps ID");
    require(virtual_display_apply_layout("system", 0), "system accepts existing arrangement");
    virtual_display_destroy();
    require(virtual_display_get_id() == 0 && waitForRemoval(id), "destroy releases VD");
    require(waitForMain(originalMain), "original main restored");
    for (uint32_t i = 0; i < count; ++i) {
      require(CGDisplayIsOnline(displays[i]), "physical screen stays online");
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "original logical origin restored");
    }
    require(CGPointEqualToPoint(CGDisplayBounds(plugged).origin, restoredPlugged), "primary teardown restores right-side local origin");
    // Keep the right-side local online for teardown from extend as well as primary.
    id = virtual_display_create(1280, 720, 60, "extend");
    require(id != 0, "extension with multiple horizontal locals");
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(plugged)), "extension starts beyond right-side local");
    virtual_display_destroy();
    require(virtual_display_get_id() == 0 && waitForRemoval(id), "extension teardown releases VD");
    require(waitForMain(originalMain), "extension teardown restores original main");
    for (uint32_t i = 0; i < count; ++i) {
      require(CGDisplayIsOnline(displays[i]), "extension teardown keeps local online");
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "extension teardown preserves local origin");
    }
    require(CGDisplayIsOnline(plugged) && CGPointEqualToPoint(CGDisplayBounds(plugged).origin, restoredPlugged),
            "extension teardown preserves right-side local origin");
    localFixture = nil;
    require(waitForRemoval(plugged), "fixture released");
    errno = 0;
    require(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD, "helper was reaped");

    id = virtual_display_create(1280, 720, 60, "extend");
    require(id != 0, "legacy create");
    uint32_t changed = virtual_display_ensure(1440, 900, 60, "extend");
    require(changed != 0, "changed mode recreates");
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(changed);
    require(mode && CGDisplayModeGetPixelWidth(mode) == 1440 && CGDisplayModeGetPixelHeight(mode) == 900,
            "changed mode uses requested pixels");
    CGDisplayModeRelease(mode);
    require(virtual_display_apply_layout("mirror", originalMain), "mirror acknowledgement");
    require(virtual_display_get_target_id() == originalMain, "mirror capture targets master");
    virtual_display_destroy();
    require(waitForRemoval(changed), "mirror cleanup");

    crashCase(argv[0], NO);
    require(waitForMain(originalMain), "parent crash restores original main");
    crashCase(argv[0], YES);
    require(waitForMain(originalMain), "stopped helper teardown restores original main");
    puts("PASS: native helper reuse, layouts, mode replacement, cleanup, reap, parent crash");
  }
  return 0;
}

#endif
