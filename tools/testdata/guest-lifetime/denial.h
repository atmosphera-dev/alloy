// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_GUEST_LIFETIME_DENIAL_H_
#define ALLOY_GUEST_LIFETIME_DENIAL_H_
#include <cassert>
#include <cstddef>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

inline void DenyLifetimeCall(int number, int error, int argument = -1, unsigned value = 0) {
  sock_filter all[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(number), 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<unsigned>(error)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  sock_filter matching[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(number), 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               static_cast<unsigned>(offsetof(seccomp_data, args) + argument * sizeof(__u64))),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<unsigned>(error)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  sock_fprog program = {static_cast<unsigned short>(argument < 0 ? 4 : 6),
                         argument < 0 ? all : matching};
  assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program));
}
#endif
