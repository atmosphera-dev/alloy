// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-only inherited policy: deny the syscall without any Snap environment.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define NATIVE_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define NATIVE_ARCH AUDIT_ARCH_AARCH64
#else
#error Unsupported seccomp fixture architecture
#endif

int main(int argc, char** argv) {
  assert(argc >= 3);
  int denied = !strcmp(argv[1], "deny");
  assert(denied || !strcmp(argv[1], "allow"));
  if (denied) {
    struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_send_signal, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog filter = {sizeof(instructions) / sizeof(instructions[0]), instructions};
    assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
    assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter));
  }
  int identity = syscall(SYS_pidfd_open, getpid(), 0);
  assert(identity >= 0);
  int result = syscall(SYS_pidfd_send_signal, identity, 0, NULL, 0);
  assert(denied ? result == -1 && errno == EPERM : result == 0);
  close(identity);
  printf("PIDFD_SIGNAL_FILTER_DENIED=%d\n", denied);
  fflush(stdout);
  execv(argv[2], argv + 2);
  return 125;
}
