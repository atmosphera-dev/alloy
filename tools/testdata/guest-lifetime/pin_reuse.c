// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Fixture-only syscall tracer, inside its own root-mapped PID namespace. It
// replaces the old spawn-then-pin target with an unrelated live sentinel.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t sentinel(void) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) for (;;) pause();
  return child;
}

static int pin(pid_t process) {
  int handle = (int)syscall(SYS_pidfd_open, process, 0);
  assert(handle >= 0);
  return handle;
}

static int live(int handle) {
  struct pollfd event = {handle, POLLIN, 0};
  assert(poll(&event, 1, 0) >= 0);
  return !event.revents;
}

static void cleanup(pid_t process, int handle) {
  int result = (int)syscall(SYS_pidfd_send_signal, handle, SIGKILL, NULL, 0);
  assert(!result || errno == ESRCH);
  int status;
  pid_t waited;
  do { waited = waitpid(process, &status, 0); } while (waited < 0 && errno == EINTR);
  assert(waited == process && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  close(handle);
}

int main(int argc, char** argv) {
  assert(argc == 4);
  if (getpid() != 1) {
    // The fixture executable has its own exact-path userns allowance. Remain
    // the unprivileged builder on the VM; root mapping is only in this private
    // test namespace, so its ns_last_pid control is isolated and writable.
    execl("/usr/bin/unshare", "unshare", "-Urpf", "--mount-proc", argv[0],
          argv[1], argv[2], argv[3], NULL);
    _exit(127);
  }
  pid_t untouched = sentinel();
  int untouched_fd = pin(untouched);
  pid_t app = fork();
  assert(app >= 0);
  if (!app) {
    assert(!ptrace(PTRACE_TRACEME, 0, NULL, NULL));
    raise(SIGSTOP);
    execl(argv[1], argv[1], argv[2], argv[3], NULL);
    _exit(127);
  }
  int status;
  assert(waitpid(app, &status, 0) == app && WIFSTOPPED(status));
  assert(!ptrace(PTRACE_SETOPTIONS, app, NULL,
                 PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
  struct user_regs_struct original;
  int replacing = 0, unsafe = 0, delivery = 0;
  pid_t replacement = -1, target = -1;
  int replacement_fd = -1;
  for (;;) {
    assert(!ptrace(PTRACE_SYSCALL, app, NULL, delivery));
    delivery = 0;
    assert(waitpid(app, &status, 0) == app);
    if (WIFEXITED(status) || WIFSIGNALED(status)) break;
    assert(WIFSTOPPED(status));
    int stopped = WSTOPSIG(status);
    if (stopped != (SIGTRAP | 0x80)) {
      if (stopped != SIGTRAP && stopped != SIGSTOP) delivery = stopped;
      continue;
    }
    struct ptrace_syscall_info call;
    assert(ptrace(PTRACE_GET_SYSCALL_INFO, app, sizeof(call), &call) >= 0);
    if (call.op == PTRACE_SYSCALL_INFO_ENTRY && !unsafe &&
        call.entry.nr == SYS_pidfd_open && call.entry.args[0] != (unsigned)app) {
      unsafe = 1;
      target = (pid_t)call.entry.args[0];
      assert(target > 1);
      assert(!ptrace(PTRACE_GETREGS, app, NULL, &original));
      int victim = pin(target);
      assert(!syscall(SYS_pidfd_send_signal, victim, SIGKILL, NULL, 0));
      close(victim);
      // Consume the original from its actual parent before reusing its PID,
      // including under default SIGCHLD. SIG_IGN/app reapers may win instead.
      struct user_regs_struct reap = original;
      reap.orig_rax = SYS_wait4;
      reap.rdi = target;
      reap.rsi = reap.rdx = reap.r10 = 0;
      assert(!ptrace(PTRACE_SETREGS, app, NULL, &reap));
      replacing = 1;
    } else if (call.op == PTRACE_SYSCALL_INFO_EXIT && replacing) {
      FILE* next = fopen("/proc/sys/kernel/ns_last_pid", "w");
      assert(next && fprintf(next, "%d\n", target - 1) > 0 && !fclose(next));
      replacement = sentinel();
      assert(replacement == target);
      replacement_fd = pin(replacement);
      // Re-execute the original pidfd_open with its original numeric argument.
      // The app's next identity-bound SIGKILL now exposes any wrong authority.
      original.rax = SYS_pidfd_open;
      original.rip -= 2;  // x86-64 syscall instruction; Alloy's pinned arch.
      assert(!ptrace(PTRACE_SETREGS, app, NULL, &original));
      replacing = 0;
    }
  }
  int good = !unsafe && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             live(untouched_fd) && waitpid(untouched, NULL, WNOHANG) == 0;
  if (replacement_fd >= 0) {
    if (!live(replacement_fd)) good = 0;
    cleanup(replacement, replacement_fd);
  }
  cleanup(untouched, untouched_fd);
  puts(good ? "PIN_IDENTITY_ATOMIC" : "PIN_REUSE_FAILURE");
  return good ? 0 : 1;
}
