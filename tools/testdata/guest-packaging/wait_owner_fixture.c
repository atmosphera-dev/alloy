// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Trusted fixture roles around the actual static wait owner, not engine code.
#define _GNU_SOURCE
#include "base/process/alloy_guest_runtime.h"
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int alloy_guest_wait_owner(const char* bootstrap);

int main(int argc, char** argv) {
  char executable[4096];
  ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  assert(size > 0);
  executable[size] = 0;
  assert(argc == 2 || argc == 3);
  if (!strcmp(argv[1], ALLOY_GUEST_WAIT_OWNER_ARGUMENT))
    return alloy_guest_wait_owner(executable);
  if (!strcmp(argv[1], ALLOY_GUEST_TEARDOWN_PROBE_ARGUMENT)) {
    if (strstr(executable, "/stopped/")) {
      // Fixture-only readiness barrier. Production bootstrap has no test hook.
      assert(write(203, "S", 1) == 1);
      assert(!raise(SIGSTOP));
    }
    assert(send(ALLOY_GUEST_CONTROL_FD, "P", 1, MSG_NOSIGNAL) == 1);
  } else {
    assert(!strcmp(argv[1], "--alloy-supervisor"));
    if (argc == 3) {
      char grant;
      ssize_t received = recv(ALLOY_GUEST_WAIT_STATUS_FD, &grant, 1, MSG_TRUNC);
      close(ALLOY_GUEST_WAIT_STATUS_FD);
      if (received == 0) return 0;
      assert(received == 1 && grant == 'G');
    }
    if (strstr(executable, "/owner-dead/")) assert(write(203, "R", 1) == 1);
  }
  char byte;
  while (recv(ALLOY_GUEST_CONTROL_FD, &byte, 1, 0) > 0) {}
  return strstr(executable, "/failed/") ? 125 : 0;
}
