// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "brave/alloy/guest/linux_lock/child_launch.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void alloy_lock_spawn_actions_destroy(posix_spawn_file_actions_t* actions,
                                     size_t count, int* copies) {
  posix_spawn_file_actions_destroy(actions);
  for (size_t i = 0; i < count; ++i)
    if (copies[i] >= 0) close(copies[i]);
}

int alloy_lock_spawn_actions(posix_spawn_file_actions_t* actions,
                            const struct alloy_lock_fd_map* maps,
                            size_t count, int* copies) {
  int maximum = 2;
  for (size_t i = 0; i < count; ++i) {
    copies[i] = -1;
    if (maps[i].target < 0 || maps[i].target == INT_MAX) return 0;
    if (maps[i].target > maximum) maximum = maps[i].target;
  }
  if (posix_spawn_file_actions_init(actions)) return 0;
  for (size_t i = 0; i < count; ++i) {
    copies[i] = fcntl(maps[i].source, F_DUPFD_CLOEXEC, maximum + 1);
    if (copies[i] < 0 ||
        posix_spawn_file_actions_adddup2(actions, copies[i], maps[i].target)) {
      alloy_lock_spawn_actions_destroy(actions, count, copies);
      return 0;
    }
  }
  return 1;
}
