// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Low-level native-consumer fixture, not a product entry point. Its synthetic
// stdout is intentionally retained; product stdio closure is tested separately.
#define _GNU_SOURCE
#include "brave/alloy/guest/linux_lock/protection.h"
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc < 2 || !alloy_lock_prepare_scratch()) return 1;
  pid_t child = fork();
  if (!child) {
    if (!alloy_lock_writer_paths() || !alloy_lock_enforce() ||
        !alloy_lock_drop_setup_capability()) _exit(2);
    execv(argv[1], argv + 1);
    _exit(3);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  alloy_lock_remove_scratch();
  return child > 0 && waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : 4;
}
