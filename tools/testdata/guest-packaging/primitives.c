// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#define _GNU_SOURCE
#include "base/process/alloy_guest_runtime.h"
#include "brave/alloy/guest/linux_lock/protection.h"
#include <assert.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  assert(!setenv("ALLOY_GUEST_RUNTIME", "1", 1));
  assert(!alloy_guest_uses_flatpak_broker());
  int fd = syscall(SYS_memfd_create, "packaging-facts-fixture", MFD_ALLOW_SEALING);
  assert(fd >= 0 && dup2(fd, ALLOY_GUEST_FACTS_FD) == ALLOY_GUEST_FACTS_FD);
  if (fd != ALLOY_GUEST_FACTS_FD) close(fd);
  struct alloy_guest_bootstrap_facts facts = {
      ALLOY_GUEST_FACTS_MAGIC, getpid(), 1, 1, 0, 1, 1};
  assert(write(ALLOY_GUEST_FACTS_FD, &facts, sizeof(facts)) == sizeof(facts));
  assert(!alloy_guest_uses_flatpak_broker());
  assert(!fcntl(ALLOY_GUEST_FACTS_FD, F_ADD_SEALS,
      F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
  assert(alloy_guest_uses_flatpak_broker());
  pid_t child = fork();
  assert(child >= 0);
  if (!child) _exit(alloy_guest_uses_flatpak_broker() ? 1 : 0);
  int status = -1;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
  close(ALLOY_GUEST_FACTS_FD);
  assert(!alloy_guest_uses_flatpak_broker());

  int pair[2];
  assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
  assert(dup2(pair[0], ALLOY_GUEST_LIVENESS_FD) == ALLOY_GUEST_LIVENESS_FD);
  if (pair[0] != ALLOY_GUEST_LIVENESS_FD) close(pair[0]);
  assert(!alloy_guest_supervisor_alive());
  signal(SIGRTMIN + 6, SIG_IGN);
  signal(SIGIO, SIG_IGN);
  assert(!fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETSIG, SIGRTMIN + 6));
  assert(!fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETOWN, getpid()));
  assert(!fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETFL, O_ASYNC | O_NONBLOCK));
  assert(alloy_guest_supervisor_alive());
  assert(send(pair[1], "unexpected", 10, MSG_NOSIGNAL) == 10);
  assert(!alloy_guest_supervisor_alive());
  char buffer[16];
  assert(recv(ALLOY_GUEST_LIVENESS_FD, buffer, sizeof(buffer), 0) == 10);
  assert(alloy_guest_supervisor_alive());
  close(pair[1]);
  assert(!alloy_guest_supervisor_alive());
  close(ALLOY_GUEST_LIVENESS_FD);

  const char* disk = getenv("TMPDIR");
  assert(disk && !alloy_lock_prepare_scratch_in(disk));
  assert(alloy_lock_prepare_scratch() && alloy_lock_scratch_valid());
  alloy_lock_remove_scratch();
  assert(!alloy_lock_scratch_valid());
  puts("GUEST_PACKAGING_PRIMITIVES_PASS");
  return 0;
}
