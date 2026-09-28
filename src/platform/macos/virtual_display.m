/** @file Native virtual display helper lifecycle. Compiled with ARC. */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include "virtual_display.h"
#include "virtual_display_mirror.h"

extern char **environ;
static pthread_mutex_t vd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pid_t vd_helper_pid;
static uint32_t vd_display_id;
static int vd_channel = -1;
static int vd_command = -1;
static int vd_width, vd_height, vd_fps;
// Only one command may be in flight. Keep its framing across bounded retries.
static BOOL vd_pending_reply, vd_reply_invalid;
static char vd_reply_buffer[32];
static size_t vd_reply_used;

static double monotonicSeconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static NSString *helperPath(void) {
  NSString *exe = [[NSBundle mainBundle] executablePath];
  if (!exe) {
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
      exe = [[NSString stringWithUTF8String:buf] stringByResolvingSymlinksInPath];
  }
  return [[exe stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"vd_helper"];
}

static BOOL validLayout(const char *layout) {
  return !layout || !*layout || !strcmp(layout, "extend") ||
         !strcmp(layout, "primary") || !strcmp(layout, "mirror") || !strcmp(layout, "system");
}

// A complete malformed line consumes its reply slot; a timeout does not.
// Overflow is drained through the newline without allocating an unbounded buffer.
static BOOL readReply(uint32_t *reply, double deadline) {
  while (vd_pending_reply) {
    double remaining = deadline - monotonicSeconds();
    if (remaining <= 0) return NO;
    struct pollfd fd = {vd_channel, POLLIN, 0};
    int ready = poll(&fd, 1, (int)(remaining * 1000) + 1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) return NO;
    char c;
    ssize_t n = read(vd_channel, &c, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n != 1) return NO;
    if (c == '\n') {
      vd_reply_buffer[vd_reply_used] = 0;
      char *end;
      errno = 0;
      unsigned long value = strtoul(vd_reply_buffer, &end, 10);
      BOOL valid = vd_reply_used && !vd_reply_invalid && !*end &&
                   errno != ERANGE && value <= UINT32_MAX;
      vd_pending_reply = vd_reply_invalid = NO;
      vd_reply_used = 0;
      if (valid) *reply = (uint32_t)value;
      return valid;
    }
    if (c < '0' || c > '9') vd_reply_invalid = YES;
    if (vd_reply_used < sizeof(vd_reply_buffer) - 1)
      vd_reply_buffer[vd_reply_used++] = c;
    else vd_reply_invalid = YES;
  }
  return NO;
}

static BOOL waitForChild(pid_t pid, double seconds) {
  double deadline = monotonicSeconds() + seconds;
  do {
    pid_t result = waitpid(pid, NULL, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) return YES;
    if (result < 0 && errno != EINTR) return NO;
    usleep(10000);
  } while (monotonicSeconds() < deadline);
  return NO;
}

// If even SIGKILL cannot be reaped within the bound, retain the pid for the next call.
// This prevents spawning another helper while the old one may still own a display.
static BOOL stopLocked(void) {
  vd_pending_reply = vd_reply_invalid = NO;
  vd_reply_used = 0;
  if (vd_command >= 0) { close(vd_command); vd_command = -1; }
  if (vd_channel >= 0) { close(vd_channel); vd_channel = -1; }
  if (vd_display_id || vd_helper_pid > 0)
    [[NSFileManager defaultManager] removeItemAtPath:@"/tmp/sunshine_vd_id" error:nil];
  vd_display_id = 0;
  if (vd_helper_pid > 0) {
    if (!waitForChild(vd_helper_pid, 2.0)) {
      kill(vd_helper_pid, SIGTERM);
      if (!waitForChild(vd_helper_pid, 1.0)) {
        kill(vd_helper_pid, SIGKILL);
        if (!waitForChild(vd_helper_pid, 1.0)) return NO;
      }
    }
    vd_helper_pid = 0;
  }
  return YES;
}

typedef enum { VD_CHILD_NONE, VD_CHILD_LIVE, VD_CHILD_UNKNOWN } vd_child_state;

// Only confirmed child death releases ownership. EINTR and visibility changes
// during WindowServer reconfiguration must not turn Resume into replacement.
static vd_child_state childStateLocked(void) {
  if (vd_helper_pid <= 0) return VD_CHILD_NONE;
  pid_t result = waitpid(vd_helper_pid, NULL, WNOHANG);
  if (result == vd_helper_pid || (result < 0 && errno == ECHILD)) {
    vd_helper_pid = 0;
    stopLocked();
    return VD_CHILD_NONE;
  }
  return result == 0 ? VD_CHILD_LIVE : VD_CHILD_UNKNOWN;
}

static BOOL listedDisplay(const CGDirectDisplayID *ids, uint32_t count, CGDirectDisplayID id) {
  for (uint32_t i = 0; i < count; ++i) if (ids[i] == id) return YES;
  return NO;
}

// Reject errors, potentially truncated lists, and topology changes between
// enumerations. Scalar online/active flags can outlive the display they describe.
static uint32_t visibleLocked(void) {
  if (!vd_display_id) return 0;
  CGDirectDisplayID online[64], active[64];
  uint32_t onlineCount = 0, activeCount = 0;
  if (CGGetOnlineDisplayList(64, online, &onlineCount) != kCGErrorSuccess || onlineCount >= 64 ||
      CGGetActiveDisplayList(64, active, &activeCount) != kCGErrorSuccess || activeCount >= 64)
    return 0;
  for (uint32_t i = 0; i < activeCount; ++i)
    if (!listedDisplay(online, onlineCount, active[i])) return 0;
  if (!listedDisplay(online, onlineCount, vd_display_id)) return 0;
  CGDirectDisplayID master = CGDisplayMirrorsDisplay(vd_display_id);
  CGDirectDisplayID target = master ? master : vd_display_id;
  if (!listedDisplay(active, activeCount, target)) return 0;
  // Software mirroring may list both members as active. The target must still
  // be the master, rather than a slave of another display.
  if (master && CGDisplayMirrorsDisplay(master)) return 0;
  return target;
}

static BOOL waitForCreationLocked(void) {
  double deadline = monotonicSeconds() + 2.0;
  uint32_t target = 0;
  size_t observedWidth = 0, observedHeight = 0;
  do {
    if (childStateLocked() != VD_CHILD_LIVE) return NO;
    target = visibleLocked();
    observedWidth = observedHeight = 0;
    // Mirror masters drive their own resolution. Independent displays must
    // expose the requested backing pixels in this process before capture starts.
    if (target && target != vd_display_id) return YES;
    if (target) {
      CGDisplayModeRef mode = CGDisplayCopyDisplayMode(vd_display_id);
      if (mode) {
        observedWidth = CGDisplayModeGetPixelWidth(mode);
        observedHeight = CGDisplayModeGetPixelHeight(mode);
        CGDisplayModeRelease(mode);
      }
      BOOL ready = observedWidth == (size_t)vd_width && observedHeight == (size_t)vd_height;
      if (ready) return YES;
    }
    // The helper's acknowledgement precedes this process's display notifications.
    if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false) == kCFRunLoopRunFinished)
      usleep(50000);
  } while (monotonicSeconds() < deadline);
  fprintf(stderr, "[virtual_display] Creation observation timed out: display=%u target=%u requestedPixels=%dx%d observedPixels=%zux%zu\n",
          vd_display_id, target, vd_width, vd_height, observedWidth, observedHeight);
  return NO;
}

static BOOL applyLocked(const char *layout, uint32_t local_main_id) {
  if (!vd_mirror_request_allowed(layout)) return NO;
  if (!validLayout(layout) || childStateLocked() != VD_CHILD_LIVE) return NO;
  double deadline = monotonicSeconds() + 5.0;
  uint32_t reply;
  if (vd_pending_reply) {
    readReply(&reply, deadline); // Consume the old response, never use it for this action.
    if (childStateLocked() != VD_CHILD_LIVE || vd_pending_reply) return NO;
  }
  if (!visibleLocked() || monotonicSeconds() >= deadline) return NO;
  if (!layout || !*layout) layout = "extend";
  char command[80];
  int size = snprintf(command, sizeof(command), "%s %u\n", layout, local_main_id);
  ssize_t sent;
  do { sent = write(vd_command, command, size); }
  while (sent < 0 && errno == EINTR && monotonicSeconds() < deadline);
  if (sent > 0) vd_pending_reply = YES;
  BOOL acknowledged = sent == size && readReply(&reply, deadline);
  if (childStateLocked() != VD_CHILD_LIVE) return NO;
  return acknowledged && reply == vd_display_id;
}

static uint32_t createLocked(int width, int height, int fps, const char *layout) {
  if (!vd_mirror_request_allowed(layout)) return 0;
  if (width <= 0 || height <= 0 || fps <= 0 || !validLayout(layout)) return 0;
  if (!stopLocked()) return 0;
  if (!layout || !*layout) layout = "extend";
  NSString *helper = helperPath();
  if (!helper || ![[NSFileManager defaultManager] isExecutableFileAtPath:helper]) return 0;

  // The command pipe is also the parent-lifetime pipe: parent SIGKILL yields EOF.
  int channel[4];
  if (pipe(channel) != 0) return 0;
  if (pipe(channel + 2) != 0) { close(channel[0]); close(channel[1]); return 0; }
  // Keep descriptors above stdio, including when the application closed stdin.
  for (int i = 0; i < 4; ++i) {
    int fd = fcntl(channel[i], F_DUPFD_CLOEXEC, 3);
    close(channel[i]);
    channel[i] = fd;
    if (fd < 0) {
      for (int j = 0; j < 4; ++j) if (channel[j] >= 0) close(channel[j]);
      return 0;
    }
  }
  // Darwin's per-descriptor flag avoids changing Sunshine's signal disposition.
  // Commands fit PIPE_BUF; a nonblocking write enqueues a whole line or fails.
  if (fcntl(channel[1], F_SETNOSIGPIPE, 1) < 0 ||
      fcntl(channel[1], F_SETFL, O_NONBLOCK) < 0) {
    for (int i = 0; i < 4; ++i) close(channel[i]);
    return 0;
  }
  char w[16], h[16], f[16];
  snprintf(w, sizeof(w), "%d", width);
  snprintf(h, sizeof(h), "%d", height);
  snprintf(f, sizeof(f), "%d", fps);
  const char *argv[] = {[helper fileSystemRepresentation], w, h, f, layout, "--managed", NULL};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, channel[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, channel[3], STDOUT_FILENO);
  for (int i = 0; i < 4; ++i) posix_spawn_file_actions_addclose(&actions, channel[i]);
  int err = posix_spawn(&vd_helper_pid, argv[0], &actions, NULL, (char *const *)argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(channel[0]);
  close(channel[3]);
  if (err) { close(channel[1]); close(channel[2]); vd_helper_pid = 0; return 0; }
  vd_command = channel[1];
  vd_channel = channel[2];
  uint32_t displayID = 0;
  vd_pending_reply = YES;
  BOOL replied = readReply(&displayID, monotonicSeconds() + 10.0);
  if (!replied || !displayID) {
    NSLog(@"[Sunshine] Virtual display startup failed: reply=%d display=%u", replied, displayID);
    stopLocked();
    return 0;
  }
  vd_display_id = displayID;
  vd_width = width; vd_height = height; vd_fps = fps;
  if (!waitForCreationLocked()) {
    stopLocked();
    return 0;
  }
  [@(displayID).stringValue writeToFile:@"/tmp/sunshine_vd_id" atomically:YES encoding:NSUTF8StringEncoding error:nil];
  return displayID;
}

uint32_t virtual_display_create(int width, int height, int fps, const char *layout) {
  pthread_mutex_lock(&vd_mutex);
  uint32_t result = createLocked(width, height, fps, layout);
  pthread_mutex_unlock(&vd_mutex);
  return result;
}

uint32_t virtual_display_ensure(int width, int height, int fps, const char *layout) {
  pthread_mutex_lock(&vd_mutex);
  uint32_t result = 0;
  if (width > 0 && height > 0 && fps > 0 && validLayout(layout) && vd_mirror_request_allowed(layout)) {
    vd_child_state state = childStateLocked();
    if (state != VD_CHILD_UNKNOWN) {
      if (state == VD_CHILD_LIVE && width == vd_width && height == vd_height && fps == vd_fps)
        result = applyLocked(layout, 0) ? vd_display_id : 0;
      else
        result = createLocked(width, height, fps, layout);
    }
  }
  pthread_mutex_unlock(&vd_mutex);
  return result;
}

int virtual_display_apply_layout(const char *layout, uint32_t local_main_id) {
  pthread_mutex_lock(&vd_mutex);
  int result = applyLocked(layout, local_main_id);
  pthread_mutex_unlock(&vd_mutex);
  return result;
}

void virtual_display_destroy(void) {
  pthread_mutex_lock(&vd_mutex);
  stopLocked();
  pthread_mutex_unlock(&vd_mutex);
}

uint32_t virtual_display_get_id(void) {
  pthread_mutex_lock(&vd_mutex);
  uint32_t result = childStateLocked() != VD_CHILD_NONE ? vd_display_id : 0;
  pthread_mutex_unlock(&vd_mutex);
  return result;
}

uint32_t virtual_display_get_target_id(void) {
  pthread_mutex_lock(&vd_mutex);
  uint32_t result = 0;
  if (childStateLocked() == VD_CHILD_LIVE) result = visibleLocked();
  pthread_mutex_unlock(&vd_mutex);
  return result;
}
