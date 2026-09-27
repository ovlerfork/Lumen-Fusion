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

extern char **environ;
static pthread_mutex_t vd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pid_t vd_helper_pid;
static uint32_t vd_display_id;
static int vd_channel = -1;
static int vd_command = -1;
static int vd_width, vd_height, vd_fps;

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

// A newline response is read with a single absolute deadline, including partial reads.
static BOOL readReply(uint32_t *reply, double timeout) {
  char buf[32];
  size_t used = 0;
  double deadline = monotonicSeconds() + timeout;
  while (used < sizeof(buf) - 1) {
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
      buf[used] = 0;
      char *end;
      unsigned long value = strtoul(buf, &end, 10);
      if (!used || *end || value > UINT32_MAX) return NO;
      *reply = (uint32_t)value;
      return YES;
    }
    buf[used++] = c;
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

static BOOL visibleLocked(void) {
  if (!vd_display_id || !CGDisplayIsOnline(vd_display_id)) return NO;
  CGDirectDisplayID master = CGDisplayMirrorsDisplay(vd_display_id);
  return CGDisplayIsActive(vd_display_id) || (master && CGDisplayIsActive(master));
}

static BOOL applyLocked(const char *layout, uint32_t local_main_id) {
  if (!validLayout(layout) || childStateLocked() != VD_CHILD_LIVE || !visibleLocked()) return NO;
  if (!layout || !*layout) layout = "extend";
  char command[80];
  int size = snprintf(command, sizeof(command), "%s %u\n", layout, local_main_id);
  ssize_t sent;
  do { sent = write(vd_command, command, size); } while (sent < 0 && errno == EINTR);
  uint32_t reply;
  if (sent != size || !readReply(&reply, 5.0)) {
    stopLocked(); // A lost acknowledgement leaves the command stream ambiguous.
    return NO;
  }
  return reply == vd_display_id;
}

static uint32_t createLocked(int width, int height, int fps, const char *layout) {
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
  if (fcntl(channel[1], F_SETNOSIGPIPE, 1) < 0) {
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
  if (!readReply(&displayID, 10.0) || !displayID) { stopLocked(); return 0; }
  vd_display_id = displayID;
  vd_width = width; vd_height = height; vd_fps = fps;
  if (childStateLocked() != VD_CHILD_LIVE || !visibleLocked()) return 0;
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
  if (width > 0 && height > 0 && fps > 0 && validLayout(layout)) {
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
  if (childStateLocked() == VD_CHILD_LIVE && visibleLocked()) {
    result = CGDisplayMirrorsDisplay(vd_display_id);
    if (!result) result = vd_display_id;
  }
  pthread_mutex_unlock(&vd_mutex);
  return result;
}
