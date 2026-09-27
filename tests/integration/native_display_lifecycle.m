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
CGError native_test_online_list(uint32_t, CGDirectDisplayID *, uint32_t *);
CGError native_test_active_list(uint32_t, CGDirectDisplayID *, uint32_t *);
CGDirectDisplayID native_test_mirror(CGDirectDisplayID);
ssize_t native_test_read(int, void *, size_t);
ssize_t native_test_write(int, const void *, size_t);
int native_test_poll(struct pollfd *, nfds_t, int);
#define waitpid native_test_waitpid
#define CGDisplayIsOnline native_test_display_online
#define CGDisplayIsActive native_test_display_active
#define CGGetOnlineDisplayList native_test_online_list
#define CGGetActiveDisplayList native_test_active_list
#define CGDisplayMirrorsDisplay native_test_mirror
#define read native_test_read
#define write native_test_write
#define poll native_test_poll
#include "virtual_display.m"
#else
extern char **environ;

// All unselected observations and helper execution remain native.
static int interruptedWait;
static pid_t observedHelper;
static CGDirectDisplayID unavailableID;
static BOOL offlineObservation, inactiveObservation;
static BOOL failNextOnlineObservation;
static CGDirectDisplayID lastReplyID, staleMaster;
static BOOL splitNextReply, delayedReply, malformedNextReply;
static int replyTimeouts, commandWrites, unlistedCreationObservations;
static unsigned long replyValue;

pid_t native_test_waitpid(pid_t pid, int *status, int options) {
  if (pid > 0) observedHelper = pid;
  if (interruptedWait) { --interruptedWait; errno = EINTR; return -1; }
  return waitpid(pid, status, options);
}
boolean_t native_test_display_online(CGDirectDisplayID id) {
  if (id == staleMaster || (id == unavailableID && offlineObservation)) return true;
  return CGDisplayIsOnline(id);
}
boolean_t native_test_display_active(CGDirectDisplayID id) {
  if (id == staleMaster || (id == unavailableID && inactiveObservation)) return true;
  return CGDisplayIsActive(id);
}
static void omitDisplay(CGDirectDisplayID *ids, uint32_t *count, CGDirectDisplayID id) {
  for (uint32_t i = 0; i < *count; ++i) {
    if (ids[i] != id) continue;
    memmove(ids + i, ids + i + 1, (*count - i - 1) * sizeof(*ids));
    --*count;
    return;
  }
}
CGError native_test_online_list(uint32_t capacity, CGDirectDisplayID *ids, uint32_t *count) {
  CGError err = CGGetOnlineDisplayList(capacity, ids, count);
  if (failNextOnlineObservation) {
    failNextOnlineObservation = NO;
    offlineObservation = YES;
    unavailableID = lastReplyID;
    fprintf(stderr, "[native_display_lifecycle] inject unlisted creation display=%u helper=%d\n",
            unavailableID, observedHelper);
  }
  if (err == kCGErrorSuccess && offlineObservation) omitDisplay(ids, count, unavailableID);
  if (err == kCGErrorSuccess && unlistedCreationObservations > 0) {
    --unlistedCreationObservations;
    omitDisplay(ids, count, lastReplyID);
  }
  return err;
}
CGError native_test_active_list(uint32_t capacity, CGDirectDisplayID *ids, uint32_t *count) {
  CGError err = CGGetActiveDisplayList(capacity, ids, count);
  if (err == kCGErrorSuccess && (offlineObservation || inactiveObservation)) omitDisplay(ids, count, unavailableID);
  return err;
}
CGDirectDisplayID native_test_mirror(CGDirectDisplayID id) {
  return staleMaster && id == unavailableID ? staleMaster : CGDisplayMirrorsDisplay(id);
}
ssize_t native_test_read(int fd, void *buffer, size_t size) {
  ssize_t n = read(fd, buffer, size);
  if (n == 1) {
    char *c = buffer;
    if (*c == '\n') {
      lastReplyID = (uint32_t)replyValue;
      replyValue = 0;
      delayedReply = NO;
    } else if (*c >= '0' && *c <= '9') replyValue = replyValue * 10 + *c - '0';
    if (splitNextReply) { splitNextReply = NO; delayedReply = YES; }
    if (malformedNextReply) { malformedNextReply = NO; *c = 'x'; }
  }
  return n;
}
ssize_t native_test_write(int fd, const void *buffer, size_t size) {
  ++commandWrites;
  return write(fd, buffer, size);
}
int native_test_poll(struct pollfd *fds, nfds_t count, int timeout) {
  // Delay delivery after a real helper reply's first byte. The remaining bytes
  // stay in the native pipe, across two separate bounded command attempts.
  if (delayedReply && replyTimeouts > 0) {
    --replyTimeouts;
    return poll(NULL, 0, timeout);
  }
  return poll(fds, count, timeout);
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
static unsigned int fixtureSerial;
static pid_t crashGroup;
static BOOL crashSignalsSent;
static CGDirectDisplayID crashDisplay;

static void logDisplays(const char *phase) {
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  CGError err = CGGetOnlineDisplayList(64, displays, &count);
  fprintf(stderr, "[native_display_lifecycle] %s pid=%d main=%u listError=%d count=%u\n",
          phase, getpid(), CGMainDisplayID(), err, count);
  if (err != kCGErrorSuccess) return;
  for (uint32_t i = 0; i < count; ++i) {
    CGDirectDisplayID id = displays[i];
    CGRect bounds = CGDisplayBounds(id);
    fprintf(stderr, "[native_display_lifecycle]   display=%u active=%d mirror=%u bounds=(%.0f,%.0f %.0fx%.0f)\n",
            id, CGDisplayIsActive(id), CGDisplayMirrorsDisplay(id),
            bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
  }
}

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
    logDisplays("failure before cleanup");
    interruptedWait = 0;
    failNextOnlineObservation = NO;
    replyTimeouts = 0;
    staleMaster = 0;
    offlineObservation = inactiveObservation = NO;
    cleanupCrash();
    virtual_display_destroy();
    localFixture = nil;
    exit(1);
  }
}

static BOOL waitForMain(CGDirectDisplayID id) {
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  CGError err = kCGErrorSuccess;
  for (int i = 0; i < 100; ++i) {
    CGDirectDisplayID cachedMain = CGMainDisplayID();
    // The active list is ordered with the main display first. Enumerate each
    // time instead of polling a scalar observation from before reconfiguration.
    count = 0;
    err = CGGetActiveDisplayList(64, displays, &count);
    if (err == kCGErrorSuccess && count > 0 && count < 64 && displays[0] == id &&
        CGPointEqualToPoint(CGDisplayBounds(id).origin, CGPointZero)) {
      if (cachedMain != id)
        fprintf(stderr, "[native_display_lifecycle] main requested=%u activeListMain=%u scalarBefore=%u scalarAfter=%u\n",
                id, displays[0], cachedMain, CGMainDisplayID());
      return YES;
    }
    // The helper configures WindowServer in another process. Service this
    // process's display notifications within the existing polling interval.
    if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false) == kCFRunLoopRunFinished)
      usleep(100000);
  }
  fprintf(stderr, "[native_display_lifecycle] main wait expired requested=%u listError=%d count=%u activeListMain=%u\n",
          id, err, count, err == kCGErrorSuccess && count ? displays[0] : 0);
  logDisplays("main wait expired");
  return NO;
}

static BOOL waitForRemoval(CGDirectDisplayID id) {
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  CGError err = kCGErrorSuccess;
  for (int i = 0; i < 100; ++i) {
    boolean_t cachedOnline = CGDisplayIsOnline(id);
    count = 0;
    err = CGGetOnlineDisplayList(64, displays, &count);
    if (err == kCGErrorSuccess && count < 64) {
      BOOL listed = NO;
      for (uint32_t j = 0; j < count; ++j) {
        if (displays[j] == id) { listed = YES; break; }
      }
      // Absence from a complete online list proves removal, including mirror
      // slaves. An API error or a potentially truncated list does not.
      if (!listed) {
        if (cachedOnline)
          fprintf(stderr, "[native_display_lifecycle] removed display=%u onlineCount=%u scalarBefore=%d scalarAfter=%d\n",
                  id, count, cachedOnline, CGDisplayIsOnline(id));
        return YES;
      }
    }
    if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false) == kCFRunLoopRunFinished)
      usleep(100000);
  }
  fprintf(stderr, "[native_display_lifecycle] removal wait expired display=%u listError=%d count=%u scalarOnline=%d\n",
          id, err, count, CGDisplayIsOnline(id));
  return NO;
}

static BOOL waitForDisplays(const CGDirectDisplayID *expected, uint32_t expectedCount, BOOL active) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    CGDirectDisplayID online[64], displays[64];
    uint32_t onlineCount = 0, count = 0;
    if (CGGetOnlineDisplayList(64, online, &onlineCount) == kCGErrorSuccess && onlineCount < 64 &&
        CGGetActiveDisplayList(64, displays, &count) == kCGErrorSuccess && count < 64) {
      BOOL foundAll = YES;
      for (uint32_t i = 0; i < expectedCount; ++i) {
        BOOL foundOnline = NO, foundActive = NO;
        for (uint32_t j = 0; j < onlineCount; ++j) foundOnline |= online[j] == expected[i];
        // Before unmirroring, an enumerated active master also makes a newly
        // registered mirror slave usable for configuration.
        CGDirectDisplayID master = !active && foundOnline ? CGDisplayMirrorsDisplay(expected[i]) : 0;
        for (uint32_t j = 0; j < count; ++j)
          foundActive |= displays[j] == expected[i] || (master && displays[j] == master);
        foundAll &= foundOnline && foundActive;
      }
      if (foundAll) return YES;
    }
    if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false) == kCFRunLoopRunFinished)
      usleep(100000);
  }
  fprintf(stderr, "[native_display_lifecycle] timed out waiting for %s display IDs:", active ? "active" : "online");
  for (uint32_t i = 0; i < expectedCount; ++i) fprintf(stderr, " %u", expected[i]);
  fputc('\n', stderr);
  return NO;
}

static CGError fixtureOrigin(CGDisplayConfigRef config, CGDirectDisplayID id, CGPoint origin) {
  CGError err = CGConfigureDisplayOrigin(config, id, (int32_t)origin.x, (int32_t)origin.y);
  fprintf(stderr, "[native_display_lifecycle] fixture origin display=%u requested=(%.0f,%.0f) error=%d\n",
          id, origin.x, origin.y, err);
  return err;
}

static CGDirectDisplayID addLocalDisplay(CGPoint origin, BOOL insertBeforeLocals) {
  logDisplays("before fixture creation");
  CGDirectDisplayID expected[64];
  CGRect before[64];
  uint32_t existingCount = 0;
  require(CGGetActiveDisplayList(64, expected, &existingCount) == kCGErrorSuccess &&
          existingCount > 0 && existingCount < 63, "fixture snapshots complete active topology");
  CGDirectDisplayID main = expected[0];
  for (uint32_t i = 0; i < existingCount; ++i) before[i] = CGDisplayBounds(expected[i]);
  CGVirtualDisplayDescriptor *descriptor = [[CGVirtualDisplayDescriptor alloc] init];
  descriptor.name = @"Native lifecycle local fixture";
  descriptor.vendorID = 0xF0F0;
  descriptor.productID = 0x5679;
  descriptor.serialNum = (unsigned int)getpid() + fixtureSerial++;
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
  fprintf(stderr, "[native_display_lifecycle] fixture=%u requestedOrigin=(%.0f,%.0f) scope=app-only\n",
          id, origin.x, origin.y);
  expected[existingCount] = id;
  require(waitForDisplays(expected, existingCount + 1, NO), "local fixture online");
  CGDisplayConfigRef config;
  CGError err = CGBeginDisplayConfiguration(&config);
  fprintf(stderr, "[native_display_lifecycle] fixture begin error=%d\n", err);
  require(err == kCGErrorSuccess, "fixture arrangement begin");
  err = CGConfigureDisplayMirrorOfDisplay(config, id, kCGNullDirectDisplay);
  fprintf(stderr, "[native_display_lifecycle] fixture unmirror display=%u error=%d\n", id, err);
  // Registration may have retiled every existing display. Configure the full
  // destination in one transaction, with the virtual main fixed at the origin.
  for (uint32_t i = 0; i < existingCount && err == kCGErrorSuccess; ++i) {
    if (CGDisplayMirrorsDisplay(expected[i])) {
      err = CGConfigureDisplayMirrorOfDisplay(config, expected[i], kCGNullDirectDisplay);
      fprintf(stderr, "[native_display_lifecycle] fixture unmirror display=%u error=%d\n", expected[i], err);
      if (err != kCGErrorSuccess) break;
    }
    if (expected[i] == main) continue;
    if (insertBeforeLocals) before[i].origin.x += 1600;
    err = fixtureOrigin(config, expected[i], before[i].origin);
  }
  if (err == kCGErrorSuccess) err = fixtureOrigin(config, id, origin);
  if (err == kCGErrorSuccess) err = fixtureOrigin(config, main, CGPointZero);
  if (err == kCGErrorSuccess) {
    err = CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
    fprintf(stderr, "[native_display_lifecycle] fixture complete error=%d\n", err);
  } else {
    CGError cancelErr = CGCancelDisplayConfiguration(config);
    fprintf(stderr, "[native_display_lifecycle] fixture cancel error=%d\n", cancelErr);
  }
  require(err == kCGErrorSuccess, "fixture positioned to the right");
  require(waitForDisplays(expected, existingCount + 1, YES), "fixture topology active");
  require(waitForMain(main), "fixture preserves virtual main");
  require(CGPointEqualToPoint(CGDisplayBounds(id).origin, origin), "fixture has exact requested origin");
  for (uint32_t i = 0; i < existingCount; ++i) {
    require(CGPointEqualToPoint(CGDisplayBounds(expected[i]).origin, before[i].origin), "fixture preserves requested existing origins");
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(expected[i])), "fixture clears every existing display");
  }
  logDisplays("after fixture configuration");
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
  crashDisplay = length > 0 ? (uint32_t)strtoul(response, NULL, 10) : 0;
  require(crashDisplay != 0, "crash parent created VD");
  if (stoppedHelper) require(kill(-crashGroup, SIGSTOP) == 0, "stop owned crash subtree");
  require(kill(parent, SIGKILL) == 0, "crash owned parent");
  BOOL released = stoppedHelper || waitForRemoval(crashDisplay);
  BOOL helperExited = stoppedHelper;
  if (!stoppedHelper && released) {
    // The helper retains a write end solely as an exit witness. Display removal
    // precedes session restoration, so removal alone must not trigger SIGKILL.
    fd.revents = 0;
    ready = poll(&fd, 1, 10000);
    char byte;
    helperExited = ready > 0 && read(output[0], &byte, 1) == 0;
  }
  close(output[0]);
  BOOL cleaned = cleanupCrash();
  require(cleaned, "bounded crash subtree cleanup and display removal");
  require(released, "parent SIGKILL releases VD through EOF");
  require(helperExited, "EOF helper finishes restoration and exits before forced cleanup");
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
      // The managed helper inherits this descriptor even though its stdout is
      // redirected to its own reply pipe. EOF observes helper exit after ours.
      int exitWitness = dup(STDOUT_FILENO);
      require(exitWitness >= 0, "crash helper exit witness");
      uint32_t id = virtual_display_ensure(1280, 720, 60, "primary");
      close(exitWitness);
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

    // The wrapper first queries online state after receiving the helper's ID.
    failNextOnlineObservation = YES;
    uint32_t failedCreation = virtual_display_ensure(1280, 720, 60, "primary");
    CGDirectDisplayID failedDisplay = unavailableID;
    offlineObservation = NO;
    require(failedCreation == 0 && !failNextOnlineObservation && unavailableID != 0,
            "acknowledged fresh creation fails visibility");
    require(observedHelper > 0, "fresh creation observed helper");
    errno = 0;
    require(waitpid(observedHelper, NULL, WNOHANG) == -1 && errno == ECHILD,
            "failed fresh creation reaps helper before returning");
    uint32_t remainingOwnedDisplay = virtual_display_get_id();
    fprintf(stderr, "[native_display_lifecycle] failed creation display=%u remainingOwnedDisplay=%u helper=%d\n",
            failedDisplay, remainingOwnedDisplay, observedHelper);
    require(remainingOwnedDisplay == 0, "failed fresh creation releases ownership");
    require(waitForRemoval(failedDisplay), "failed fresh creation removes display");
    require(waitForMain(originalMain), "failed fresh creation restores original main");

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
    splitNextReply = YES;
    replyTimeouts = 2;
    int writesBeforeDelay = commandWrites;
    require(!virtual_display_apply_layout("extend", originalMain), "partial acknowledgement times out");
    require(delayedReply && virtual_display_get_id() == id && observedHelper == originalHelper,
            "acknowledgement timeout retains live helper and VD");
    require(waitForMain(originalMain), "timed-out command actually applied extension");
    require(!virtual_display_apply_layout("primary", 0), "outstanding reply keeps retry bounded");
    require(commandWrites == writesBeforeDelay + 1 && virtual_display_get_id() == id,
            "pending acknowledgement prevents another command without releasing VD");
    require(virtual_display_apply_layout("primary", 0) && waitForMain(id), "late partial reply drained before primary recovery");
    require(!delayedReply && virtual_display_get_id() == id && observedHelper == originalHelper,
            "delayed acknowledgement recovery retains same helper and VD");
    malformedNextReply = YES;
    require(!virtual_display_apply_layout("extend", originalMain), "malformed acknowledgement is uncertain");
    require(virtual_display_get_id() == id && waitForMain(originalMain), "malformed acknowledgement retains applied desktop");
    require(virtual_display_apply_layout("primary", 0) && observedHelper == originalHelper,
            "malformed complete reply permits recovery on same helper");
    require(waitForMain(id), "primary ready before retiled hotplug");
    CGPoint beforeRetile = CGDisplayBounds(originalMain).origin;
    CGRect beforeRetileBounds[64];
    CGFloat localLeft = beforeRetile.x;
    for (uint32_t i = 0; i < count; ++i) {
      beforeRetileBounds[i] = CGDisplayBounds(displays[i]);
      localLeft = MIN(localLeft, beforeRetileBounds[i].origin.x);
    }
    // Reproduce WindowServer inserting a display between the virtual main and
    // existing locals, displacing those locals by the new display's width.
    CGPoint insertedOrigin = CGPointMake(localLeft, beforeRetile.y);
    CGDirectDisplayID inserted = addLocalDisplay(insertedOrigin, YES);
    CGPoint restoredInserted = CGPointMake(insertedOrigin.x - beforeRetile.x - 1600,
                                           insertedOrigin.y - beforeRetile.y);
    require(virtual_display_apply_layout("primary", 0) && waitForMain(id), "primary observes retiled hotplug");
    require(virtual_display_get_id() == id && observedHelper == originalHelper, "retiled primary keeps helper and ID");
    require(CGPointEqualToPoint(CGDisplayBounds(inserted).origin, insertedOrigin), "retiled primary preserves new local origin");
    for (uint32_t i = 0; i < count; ++i) {
      CGPoint expected = beforeRetileBounds[i].origin;
      expected.x += 1600;
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, expected), "retiled primary preserves local arrangement");
      require(!CGRectIntersectsRect(CGDisplayBounds(inserted), CGDisplayBounds(displays[i])), "retiled primary has no local collisions");
    }
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(inserted)), "retiled primary clears new local");
    require(virtual_display_apply_layout("extend", originalMain) && waitForMain(originalMain), "retiled extension restores local main");
    require(virtual_display_get_id() == id && observedHelper == originalHelper, "retiled extension keeps helper and ID");
    require(CGPointEqualToPoint(CGDisplayBounds(inserted).origin, restoredInserted), "retiled hotplug normalized against observed local frame");
    for (uint32_t i = 0; i < count; ++i) {
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "retiled extension restores local origins");
      require(!CGRectIntersectsRect(CGDisplayBounds(inserted), CGDisplayBounds(displays[i])), "retiled extension has no local collisions");
      require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(displays[i])), "retiled extension clears existing locals");
    }
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(inserted)), "retiled extension clears new local");
    virtual_display_destroy();
    errno = 0;
    require(waitpid(originalHelper, NULL, WNOHANG) == -1 && errno == ECHILD,
            "retiled helper reaped before final layout observations");
    require(virtual_display_get_id() == 0 && waitForRemoval(id), "retiled teardown releases VD");
    require(waitForMain(originalMain), "retiled teardown restores main");
    require(CGPointEqualToPoint(CGDisplayBounds(inserted).origin, restoredInserted), "retiled teardown preserves new local origin");
    for (uint32_t i = 0; i < count; ++i)
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "retiled teardown preserves existing origins");
    localFixture = nil;
    require(waitForRemoval(inserted), "retiled fixture released");
    id = virtual_display_ensure(1280, 720, 60, "primary");
    require(id != 0 && waitForMain(id), "primary ready for right-side hotplug");
    CGPoint translation = CGDisplayBounds(originalMain).origin;
    CGFloat right = 0;
    for (uint32_t i = 0; i < count; ++i) right = MAX(right, CGRectGetMaxX(CGDisplayBounds(displays[i])));
    CGPoint pluggedOrigin = CGPointMake(right, translation.y);
    CGDirectDisplayID plugged = addLocalDisplay(pluggedOrigin, NO);
    CGPoint restoredPlugged = CGPointMake(pluggedOrigin.x - translation.x, pluggedOrigin.y - translation.y);
    fprintf(stderr, "[native_display_lifecycle] fixture=%u expectedRestoredOrigin=(%.0f,%.0f) translation=(%.0f,%.0f)\n",
            plugged, restoredPlugged.x, restoredPlugged.y, translation.x, translation.y);
    require(virtual_display_apply_layout("primary", 0), "primary observes hotplug");
    require(waitForMain(id), "hotplug primary observed");
    CGRect pluggedPrimary = CGDisplayBounds(plugged);
    require(virtual_display_apply_layout("primary", 0), "repeated primary");
    require(waitForMain(id), "repeated primary observed");
    require(CGPointEqualToPoint(CGDisplayBounds(plugged).origin, pluggedPrimary.origin), "hotplug primary is idempotent");
    logDisplays("before extension request");
    require(virtual_display_apply_layout("extend", originalMain), "extension acknowledgement");
    logDisplays("extension acknowledged before servicing run loop");
    require(virtual_display_get_id() == id && waitForMain(originalMain), "extension keeps VD and restores local main");
    require(CGPointEqualToPoint(CGDisplayBounds(plugged).origin, restoredPlugged), "hotplug normalized into original frame");
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(plugged)), "extension clears right-side local display");
    for (uint32_t i = 0; i < count; ++i) {
      require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(displays[i])), "extension clears every local display");
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "extension preserves local arrangement");
    }
    require(virtual_display_apply_layout("extend", originalMain), "repeated extension");
    require(waitForMain(originalMain), "repeated extension observed");
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
    // Populate the parent's old mode cache before the helper replaces its VD.
    CGDisplayModeRef oldMode = CGDisplayCopyDisplayMode(id);
    require(oldMode && CGDisplayModeGetPixelWidth(oldMode) == 1280 && CGDisplayModeGetPixelHeight(oldMode) == 720,
            "mode replacement starts with observed old pixels");
    CGDisplayModeRelease(oldMode);
    pid_t oldHelper = observedHelper;
    // The parent can briefly enumerate a topology without the new helper's ID.
    unlistedCreationObservations = 2;
    uint32_t changed = virtual_display_ensure(1440, 900, 60, "extend");
    require(changed != 0 && observedHelper != oldHelper, "changed mode recreates");
    require(unlistedCreationObservations == 0, "replacement waits through stale parent enumeration");
    errno = 0;
    require(waitpid(oldHelper, NULL, WNOHANG) == -1 && errno == ECHILD, "mode replacement reaps old helper");
    require(virtual_display_get_target_id() == changed, "replacement is listed and capturable before returning");
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
    for (uint32_t i = 0; i < count; ++i)
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "parent EOF preserves local origins after helper exit");
    crashCase(argv[0], YES);
    require(waitForMain(originalMain), "stopped helper teardown restores original main");

    id = virtual_display_create(1280, 720, 60, "primary");
    require(id != 0 && waitForMain(id), "primary for local replacement");
    pid_t replacementHelper = observedHelper;
    right = 0;
    for (uint32_t i = 0; i < count; ++i) right = MAX(right, CGRectGetMaxX(CGDisplayBounds(displays[i])));
    CGDirectDisplayID retiredLocal = addLocalDisplay(CGPointMake(right, 0), NO);
    require(virtual_display_apply_layout("primary", retiredLocal) && waitForMain(id), "helper remembers local before removal");
    localFixture = nil;
    require(waitForRemoval(retiredLocal), "remembered local removed");
    unavailableID = id;
    staleMaster = retiredLocal;
    require(native_test_display_online(retiredLocal) && native_test_display_active(retiredLocal),
            "removed master retains injected stale scalar flags");
    require(virtual_display_get_target_id() == 0 && virtual_display_get_id() == id,
            "removed stale mirror master is not a target and does not release owned VD");
    staleMaster = 0;
    require(virtual_display_get_target_id() == id, "fresh target recovers after stale master observation");
    require(waitForMain(id), "primary remains after local removal");
    translation = CGDisplayBounds(originalMain).origin;
    right = 0;
    for (uint32_t i = 0; i < count; ++i) right = MAX(right, CGRectGetMaxX(CGDisplayBounds(displays[i])));
    CGDirectDisplayID replacement = addLocalDisplay(CGPointMake(right, translation.y), NO);
    require(replacement != retiredLocal, "replacement has distinct native identity");
    CGPoint replacementOrigin = CGPointMake(right - translation.x, 0);
    require(virtual_display_apply_layout("extend", retiredLocal), "removed preferred local is skipped");
    require(waitForMain(originalMain), "listed local becomes main after removal");
    require(virtual_display_get_id() == id && observedHelper == replacementHelper, "local replacement retains helper and VD");
    require(CGPointEqualToPoint(CGDisplayBounds(replacement).origin, replacementOrigin), "listed replacement keeps normalized origin");
    require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(replacement)), "extension clears listed replacement");
    for (uint32_t i = 0; i < count; ++i) {
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, bounds[i].origin), "local removal preserves remaining origins");
      require(!CGRectIntersectsRect(CGDisplayBounds(id), CGDisplayBounds(displays[i])), "local removal leaves VD clear of locals");
    }
    virtual_display_destroy();
    require(waitForRemoval(id) && waitForMain(originalMain), "replacement session cleanup");

    // Keep two locals online so an arrangement change in a system-only session
    // is observable after the helper exits and its own display disappears.
    id = virtual_display_create(1280, 720, 60, "system");
    require(id != 0, "system-only session created");
    pid_t systemHelper = observedHelper;
    CGDirectDisplayID userDisplays[64];
    CGRect userBounds[64];
    for (uint32_t i = 0; i < count; ++i) userDisplays[i] = displays[i];
    userDisplays[count] = replacement;
    userDisplays[count + 1] = id;
    require(waitForDisplays(userDisplays, count + 2, NO), "system-only topology enumerated");
    CGFloat userLeft = CGDisplayBounds(displays[0]).origin.x;
    for (uint32_t i = 0; i < count; ++i) {
      userBounds[i] = CGDisplayBounds(displays[i]);
      userLeft = MIN(userLeft, userBounds[i].origin.x);
    }
    CGFloat userOffset = CGDisplayBounds(replacement).size.width - userLeft;
    CGDisplayConfigRef userConfig;
    CGError userErr = CGBeginDisplayConfiguration(&userConfig);
    fprintf(stderr, "[native_display_lifecycle] user arrangement begin error=%d\n", userErr);
    require(userErr == kCGErrorSuccess, "user arrangement begin");
    for (uint32_t i = 0; i < count + 2 && userErr == kCGErrorSuccess; ++i) {
      if (!CGDisplayMirrorsDisplay(userDisplays[i])) continue;
      userErr = CGConfigureDisplayMirrorOfDisplay(userConfig, userDisplays[i], kCGNullDirectDisplay);
      fprintf(stderr, "[native_display_lifecycle] user unmirror display=%u error=%d\n", userDisplays[i], userErr);
    }
    CGFloat userRight = 0;
    for (uint32_t i = 0; i < count && userErr == kCGErrorSuccess; ++i) {
      userBounds[i].origin.x += userOffset;
      userRight = MAX(userRight, CGRectGetMaxX(userBounds[i]));
      userErr = fixtureOrigin(userConfig, displays[i], userBounds[i].origin);
    }
    if (userErr == kCGErrorSuccess) userErr = fixtureOrigin(userConfig, id, CGPointMake(userRight, 0));
    if (userErr == kCGErrorSuccess) userErr = fixtureOrigin(userConfig, replacement, CGPointZero);
    if (userErr == kCGErrorSuccess) userErr = CGCompleteDisplayConfiguration(userConfig, kCGConfigureForSession);
    else {
      CGError cancelErr = CGCancelDisplayConfiguration(userConfig);
      fprintf(stderr, "[native_display_lifecycle] user arrangement cancel error=%d\n", cancelErr);
    }
    fprintf(stderr, "[native_display_lifecycle] user arrangement scope=session completion=%d\n", userErr);
    require(userErr == kCGErrorSuccess && waitForMain(replacement), "user selects a different local main during system-only session");
    require(waitForDisplays(userDisplays, count + 2, YES), "user arrangement active");
    for (uint32_t i = 0; i < count; ++i)
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, userBounds[i].origin), "user arrangement has exact requested local origins");
    virtual_display_destroy();
    errno = 0;
    require(waitpid(systemHelper, NULL, WNOHANG) == -1 && errno == ECHILD, "system-only helper reaped");
    require(virtual_display_get_id() == 0 && waitForRemoval(id), "system-only VD released");
    require(waitForMain(replacement), "system-only cleanup preserves user-selected main");
    for (uint32_t i = 0; i < count; ++i) {
      require(CGPointEqualToPoint(CGDisplayBounds(displays[i]).origin, userBounds[i].origin), "system-only cleanup preserves user origins");
      require(!CGRectIntersectsRect(CGDisplayBounds(replacement), CGDisplayBounds(displays[i])), "system-only cleanup preserves nonoverlap");
    }
    localFixture = nil;
    require(waitForRemoval(replacement), "replacement fixture released");
    require(waitForMain(originalMain), "original main remains after fixture cleanup");
    puts("PASS: native helper reuse, layouts, mode replacement, cleanup, reap, parent crash, local replacement, system-only arrangement");
  }
  return 0;
}

#endif
