// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_CHILD_LAUNCH_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_CHILD_LAUNCH_H_
#include <spawn.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct alloy_lock_fd_map { int source; int target; };
// Freeze every source above every destination before constructing actions.
// No action can overwrite a later source, including supervisor-owned channels.
int alloy_lock_spawn_actions(posix_spawn_file_actions_t* actions,
                            const struct alloy_lock_fd_map* maps,
                            size_t count, int* copies);
void alloy_lock_spawn_actions_destroy(posix_spawn_file_actions_t* actions,
                                     size_t count, int* copies);
#ifdef __cplusplus
}
#endif
#endif
