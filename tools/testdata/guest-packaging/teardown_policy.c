// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Actual static-supervisor ownership and error propagation, without an engine.
#define _GNU_SOURCE
#include "brave/alloy/guest/bootstrap/supervisor.c"
#include "pidfd_policy.h"
#include <stdio.h>

static void retirement_case(int denied) {
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
  int handle = (int)syscall(SYS_pidfd_open, child, 0);
  assert(handle >= 0);
  if (denied) deny_pidfd_signaling(0);
  assert(terminate_identity(handle) == !denied);
  struct members members = {malloc(sizeof(struct member)), 1};
  assert(members.items);
  members.items[0] = (struct member){handle, release[1], child};
  // EOF releases only this fixture's trusted child when the filtered kill is
  // denied. The product still owns every pidfd until exit; no fallback kill.
  assert(retire(&members) == !denied);
  assert(!members.items && !members.count);
  assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
}

int main(void) {
  for (int denied = 0; denied != 2; ++denied) {
    pid_t worker = fork();
    assert(worker >= 0);
    if (!worker) {
      retirement_case(denied);
      _exit(0);
    }
    int status;
    assert(waitpid(worker, &status, 0) == worker && status == 0);
  }
  puts("GUEST_TEARDOWN_ERROR_REPORTED=1");
  return 0;
}
