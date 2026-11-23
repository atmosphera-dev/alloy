// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "brave/alloy/guest/linux_lock/protection.h"
#include "brave/alloy/guest/linux_lock/scratch_mount.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

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
    if (lstat(ALLOY_GUEST_SCRATCH_PATH, &name) || fstat(owned_fd, &pin) ||
        name.st_dev != pin.st_dev || name.st_ino != pin.st_ino)
      return 0;
  }
  return fd >= 0 && !fstat(fd, &st) && !fstatfs(fd, &fs) &&
      S_ISDIR(st.st_mode) && st.st_uid == getuid() && st.st_nlink > 0 &&
      (st.st_mode & 0777) == 0700 && fs.f_type == TMPFS_MAGIC && fs.f_bavail > 0 &&
      (fs.f_flags & (ST_NOSUID | ST_NODEV | ST_NOEXEC)) == (ST_NOSUID | ST_NODEV | ST_NOEXEC) &&
      (unsigned long long)fs.f_blocks * fs.f_bsize <= ALLOY_GUEST_SCRATCH_LIMIT &&
      (!fs.f_files || fs.f_ffree > 0);
}

int alloy_lock_prepare_scratch(void) {
  // The raw enclosure setup owns mount creation. Borrow only that verified
  // private tmpfs; never create a named shared directory or a disk fallback.
  int fd = open(ALLOY_GUEST_SCRATCH_PATH, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return 0;
  owned_fd = fcntl(fd, F_DUPFD, 256);
  close(fd);
  char value[32];
  snprintf(value, sizeof(value), "%d", owned_fd);
  if (owned_fd < 0 || setenv("ALLOY_GUEST_SCRATCH_FD", value, 1) ||
      !alloy_lock_scratch_valid())
    return 0;
  return 1;
}

void alloy_lock_remove_scratch(void) {
  // Release the supervisor's pin only. Kernel mount/reference lifetime owns
  // reclamation even if this process is SIGKILLed or permissions are damaged.
  if (owned_fd < 0) return;
  close(owned_fd);
  owned_fd = -1;
  unsetenv("ALLOY_GUEST_SCRATCH_FD");
}
