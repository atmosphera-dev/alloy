// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-owned pre-exec policy, never linked into the product.
#include "denial.h"
#include <cerrno>
#include <cstring>
#include <signal.h>
#include <unistd.h>

int main(int argc, char** argv) {
  assert(argc >= 3);
  if (!strcmp(argv[1], "clone")) DenyLifetimeCall(SYS_clone3, ENOSYS);
  else if (!strcmp(argv[1], "wait")) DenyLifetimeCall(SYS_waitid, EPERM, 0, 3);
  else if (!strcmp(argv[1], "kill")) DenyLifetimeCall(SYS_pidfd_send_signal, EPERM, 1, SIGKILL);
  else return 2;
  execv(argv[2], argv + 2);
  return 127;
}
