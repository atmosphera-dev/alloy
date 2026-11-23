// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_SCRATCH_MOUNT_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_SCRATCH_MOUNT_H_
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ALLOY_GUEST_SCRATCH_PATH "/dev/shm"
#define ALLOY_GUEST_SCRATCH_LIMIT (1024ULL * 1024 * 1024)

// Called only in a fresh private mount namespace, before fixed exec/Landlock.
// A fixed mount point is private to that namespace, not a shared directory.
static inline int alloy_lock_mount_scratch(void) {
  return (int)syscall(SYS_mount, "alloy-scratch", ALLOY_GUEST_SCRATCH_PATH, "tmpfs",
                     MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=1073741824,mode=0700");
}
#endif
