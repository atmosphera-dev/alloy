// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "brave/alloy/guest/linux_lock/protection.h"
#include "brave/alloy/guest/linux_lock/linux_uapi.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

static int grant(int ruleset, const char* path, unsigned long long rights) {
  int fd = open(path, O_PATH | O_CLOEXEC);
  if (fd < 0) return 0;
  struct landlock_path_beneath_attr rule = {.allowed_access = rights, .parent_fd = fd};
  int result = syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
  close(fd);
  return result == 0;
}

int alloy_lock_enforce(void) {
  int abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 3 || !alloy_lock_scratch_valid()) return 0;
  unsigned long long rights = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
      LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
      LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
      LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
      LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
      LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER;
  // Do not grant MAKE_CHAR/MAKE_BLOCK even on RAM: anonymous memory and the
  // explicitly listed existing virtual devices suffice. No disk-device root.
  struct landlock_ruleset_attr attr = {.handled_access_fs = rights};
  int ruleset = syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
  if (ruleset < 0) return 0;
  struct landlock_path_beneath_attr ram = {
      .allowed_access = rights & ~(LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_BLOCK),
      .parent_fd = alloy_lock_scratch_fd()};
  int ok = !syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &ram, 0) &&
      grant(ruleset, "/proc", LANDLOCK_ACCESS_FS_WRITE_FILE) &&
      grant(ruleset, "/dev/null", LANDLOCK_ACCESS_FS_WRITE_FILE) &&
      grant(ruleset, "/dev/urandom", LANDLOCK_ACCESS_FS_WRITE_FILE) &&
      grant(ruleset, "/dev/random", LANDLOCK_ACCESS_FS_WRITE_FILE) &&
      !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) &&
      !syscall(SYS_landlock_restrict_self, ruleset, 0);
  close(ruleset);
  if (!ok) return 0;
  char value[32];
  snprintf(value, sizeof(value), "%d", abi);
  return !setenv("ALLOY_GUEST_LOCK_ABI", value, 1);
}
