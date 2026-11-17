// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_LINUX_UAPI_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_LINUX_UAPI_H_
#include <linux/landlock.h>
#include <sys/syscall.h>

// Chromium's hermetic sysroot predates ABI 3, independently of the runtime
// kernel. These are the stable Linux UAPI values, not an availability probe.
// See Linux v6.2 include/uapi/linux/landlock.h and Chromium's pinned
// sandbox/linux/system_headers/x86_64_linux_syscalls.h.
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif

#if !defined(SYS_landlock_create_ruleset) || !defined(SYS_landlock_add_rule) || \
    !defined(SYS_landlock_restrict_self)
#if defined(__x86_64__) && !defined(__ILP32__)
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif
#else
#error "Landlock syscall UAPI definitions are required for this architecture"
#endif
#endif
#endif
