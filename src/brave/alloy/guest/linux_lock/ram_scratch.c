// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "brave/alloy/guest/linux_lock/protection.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static char owned_path[PATH_MAX];
static int owned_fd = -1;

int alloy_lock_scratch_fd(void) {
  const char* value = getenv("ALLOY_GUEST_SCRATCH_FD");
  if (!value || !*value)
    return -1;
  char* end;
  errno = 0;
  long fd = strtol(value, &end, 10);
  return !errno && !*end && fd >= 256 && fd <= INT_MAX ? (int)fd : -1;
}

int alloy_lock_preserves(int fd) {
  return fd == alloy_lock_scratch_fd();
}

int alloy_lock_drop_setup_capability(void) {
  int fd = alloy_lock_scratch_fd();
  if (fd >= 0 && close(fd)) return 0;
  return !unsetenv("ALLOY_GUEST_SCRATCH_FD");
}

int alloy_lock_scratch_valid(void) {
  int fd = alloy_lock_scratch_fd();
  struct stat st;
  struct statfs fs;
  if (owned_fd >= 0) {
    struct stat name, pin;
    if (lstat(owned_path, &name) || fstat(owned_fd, &pin) ||
        name.st_dev != pin.st_dev || name.st_ino != pin.st_ino)
      return 0;
  }
  return fd >= 0 && !fstat(fd, &st) && !fstatfs(fd, &fs) &&
      S_ISDIR(st.st_mode) && st.st_uid == getuid() && st.st_nlink > 0 &&
      (st.st_mode & 0777) == 0700 && fs.f_type == TMPFS_MAGIC && fs.f_bavail > 0 &&
      (!fs.f_files || fs.f_ffree > 0);
}

int alloy_lock_prepare_scratch(void) {
  // Do not trust HOME, TMPDIR, XDG_RUNTIME_DIR, or a pathname that merely looks
  // like a runtime directory. No disk-possible alternative is selected.
  int parent = open("/dev/shm", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct statfs fs;
  struct stat before, after;
  if (parent < 0 || fstatfs(parent, &fs) || fs.f_type != TMPFS_MAGIC)
    return 0;
  if (snprintf(owned_path, sizeof(owned_path), "/dev/shm/alloy-guest-%lu-XXXXXX",
               (unsigned long)getuid()) >= (int)sizeof(owned_path) ||
      !mkdtemp(owned_path)) {
    close(parent);
    return 0;
  }
  const char* name = strrchr(owned_path, '/') + 1;
  int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 || fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW) ||
      fstat(fd, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
    if (fd >= 0) close(fd);
    close(parent);
    return 0;
  }
  close(parent);
  owned_fd = fcntl(fd, F_DUPFD, 256);
  close(fd);
  char value[32];
  snprintf(value, sizeof(value), "%d", owned_fd);
  if (owned_fd < 0 || setenv("ALLOY_GUEST_SCRATCH_FD", value, 1) ||
      !alloy_lock_scratch_valid())
    return 0;
  return 1;
}

static void remove_children(int fd) {
  // Scratch loss can include permission damage. This exact pinned, owned RAM
  // directory is no longer admitted to an engine; restore cleanup authority.
  if (fchmod(fd, 0700)) return;
  int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  DIR* directory = copy >= 0 ? fdopendir(copy) : NULL;
  if (!directory) {
    if (copy >= 0) close(copy);
    return;
  }
  struct dirent* entry;
  while ((entry = readdir(directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    struct stat st;
    if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
    if (S_ISDIR(st.st_mode)) {
      // O_PATH pins even a mode-000 directory without following symlinks.
      // Restore access through that pin, never through the mutable entry name.
      int pin = openat(fd, entry->d_name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (pin < 0) continue;
      char path[64];
      snprintf(path, sizeof(path), "/proc/self/fd/%d", pin);
      int child = -1;
      if (!chmod(path, 0700))
        child = openat(pin, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      close(pin);
      if (child >= 0) {
        remove_children(child);
        close(child);
        if (unlinkat(fd, entry->d_name, AT_REMOVEDIR)) continue;
      }
    } else if (unlinkat(fd, entry->d_name, 0)) {
      continue;
    }
  }
  closedir(directory);
}

void alloy_lock_remove_scratch(void) {
  if (owned_fd < 0) return;
  struct stat descriptor, path;
  if (!fstat(owned_fd, &descriptor) && !lstat(owned_path, &path) &&
      path.st_dev == descriptor.st_dev && path.st_ino == descriptor.st_ino) {
    remove_children(owned_fd);
    if (rmdir(owned_path)) { /* Disclosed stale volatile scratch, never reused. */ }
  }
  close(owned_fd);
  owned_fd = -1;
}
