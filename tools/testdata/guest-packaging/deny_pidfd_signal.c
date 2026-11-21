// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-only inherited policy: deny the syscall without any Snap environment.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pidfd_policy.h"

int main(int argc, char** argv) {
  assert(argc >= 3);
  int deny_all = !strcmp(argv[1], "deny");
  int deny_kill = !strcmp(argv[1], "deny-kill");
  assert(deny_all || deny_kill || !strcmp(argv[1], "allow"));
  if (deny_all || deny_kill) deny_pidfd_signaling(deny_all);
  int identity = syscall(SYS_pidfd_open, getpid(), 0);
  assert(identity >= 0);
  int result = syscall(SYS_pidfd_send_signal, identity, 0, NULL, 0);
  assert(deny_all ? result == -1 && errno == EPERM : result == 0);
  close(identity);
  if (deny_kill) {
    int release[2];
    assert(!pipe(release));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      close(release[1]);
      char byte;
      ssize_t count;
      do { count = read(release[0], &byte, 1); } while (count < 0 && errno == EINTR);
      _exit(count == 0 ? 0 : 125);
    }
    close(release[0]);
    identity = syscall(SYS_pidfd_open, child, 0);
    assert(identity >= 0);
    assert(syscall(SYS_pidfd_send_signal, identity, SIGKILL, NULL, 0) == -1 && errno == EPERM);
    close(release[1]);
    int status;
    assert(waitpid(child, &status, 0) == child && status == 0);
    close(identity);
  }
  printf("PIDFD_SIGNAL_FILTER_DENIED=%d\n", deny_all || deny_kill);
  fflush(stdout);
  execv(argv[2], argv + 2);
  return 125;
}
