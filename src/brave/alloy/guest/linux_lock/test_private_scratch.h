// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-only setup for low-level Landlock consumers, not a product entry route.
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_TEST_PRIVATE_SCRATCH_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_TEST_PRIVATE_SCRATCH_H_
#include "brave/alloy/guest/linux_lock/scratch_mount.h"
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>

static int alloy_test_map(const char* path, const char* value) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  size_t size = 0;
  while (value[size]) ++size;
  int result = write(fd, value, size) == (ssize_t)size;
  return close(fd) == 0 && result;
}

static int alloy_test_private_scratch(void) {
  char uid[96], gid[96];
  snprintf(uid, sizeof(uid), "%u %u 1\n", (unsigned)getuid(), (unsigned)getuid());
  snprintf(gid, sizeof(gid), "%u %u 1\n", (unsigned)getgid(), (unsigned)getgid());
  return !unshare(CLONE_NEWUSER | CLONE_NEWNS) &&
      alloy_test_map("/proc/self/setgroups", "deny\n") &&
      alloy_test_map("/proc/self/uid_map", uid) && alloy_test_map("/proc/self/gid_map", gid) &&
      !mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) && !alloy_lock_mount_scratch();
}
#endif
