// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_TEST_PIDFD_POLICY_H_
#define ALLOY_TEST_PIDFD_POLICY_H_
#include <assert.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#if defined(__x86_64__)
#define ALLOY_TEST_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ALLOY_TEST_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error Unsupported seccomp fixture architecture
#endif

static inline void deny_pidfd_signaling(int deny_all) {
  struct sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ALLOY_TEST_AUDIT_ARCH, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_send_signal, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIGKILL, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  if (deny_all)
    instructions[6] = (struct sock_filter)BPF_STMT(BPF_JMP | BPF_JA, 0);
  struct sock_fprog filter = {sizeof(instructions) / sizeof(instructions[0]), instructions};
  assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter));
}
#endif
