// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Actual static-supervisor ownership and error propagation, without an engine.
#define _GNU_SOURCE
#include "brave/alloy/guest/bootstrap/supervisor.c"
#include "pidfd_policy.h"
#include <stdio.h>

// A child that only an actual signal can end; EOF on its pipe is ignored.
static pid_t signal_only_child(int release[2]) {
  assert(!pipe(release));
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    close(release[1]);
    close(release[0]);
    for (;;) pause();
  }
  close(release[0]);
  return child;
}

// A trusted fixture child that EOF releases when signaling is denied.
static pid_t eof_child(int release[2]) {
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
  return child;
}

static void terminate_case(int denied) {
  int release[2];
  pid_t child = denied ? eof_child(release) : signal_only_child(release);
  int handle = (int)syscall(SYS_pidfd_open, child, 0);
  assert(handle >= 0);
  if (denied) deny_pidfd_signaling(0);
  assert(terminate_identity(handle) == !denied);
  close(release[1]);
  int status;
  assert(waitpid(child, &status, 0) == child);
  if (denied)
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  else
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  close(handle);
}

static void retirement_case(int denied) {
  int release[2];
  // Allowed: only retire()'s own SIGKILL can end this child. Denied: EOF
  // releases only this fixture's trusted child after the filtered kill fails.
  // The product still owns every pidfd until exit; no fallback kill.
  pid_t child = denied ? eof_child(release) : signal_only_child(release);
  int handle = (int)syscall(SYS_pidfd_open, child, 0);
  assert(handle >= 0);
  if (denied) deny_pidfd_signaling(0);
  struct members members = {malloc(sizeof(struct member)), 1};
  assert(members.items);
  members.items[0] = (struct member){handle, release[1], child};
  assert(retire(&members) == !denied);
  assert(!members.items && !members.count);
  assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void isolated(void (*test)(int), int denied) {
  pid_t worker = fork();
  assert(worker >= 0);
  if (!worker) {
    test(denied);
    _exit(0);
  }
  int status;
  assert(waitpid(worker, &status, 0) == worker && status == 0);
}

int main(void) {
  for (int denied = 0; denied != 2; ++denied) {
    isolated(terminate_case, denied);
    isolated(retirement_case, denied);
  }
  puts("GUEST_TEARDOWN_ERROR_REPORTED=1");
  return 0;
}
