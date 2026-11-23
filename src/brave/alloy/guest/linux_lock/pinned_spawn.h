// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_PINNED_SPAWN_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_PINNED_SPAWN_H_
#include "brave/alloy/guest/linux_lock/child_launch.h"
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

struct alloy_pinned_child {
  pid_t process;
  int pidfd;
  int exec_error;
  const char* operation;
};

// No spawn-then-pin or clone fallback. Parent prepares every input; the copied
// child executes only async-signal-safe C setup and the fixed exec. The returned
// error lane reports setup/exec errno (EOF means exec), without waiting before
// the caller has installed custody of the returned pidfd.
int alloy_spawn_pinned(const char* path, char* const argv[], char* const envp[],
                       const struct alloy_lock_fd_map* maps, size_t count,
                       int enclosure, struct alloy_pinned_child* child);
int alloy_pinned_exec_result(int error_fd, const char** operation);
int alloy_pinned_signal(int pidfd, int signal_number);
int alloy_pinned_wait(int pidfd, int* status);
int alloy_pinned_exited(int pidfd, int blocking);

#ifdef __cplusplus
}
#endif
#endif
