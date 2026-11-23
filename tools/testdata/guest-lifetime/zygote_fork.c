// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-only root tracer: hold an actual zygote newborn before any user code.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int waited(pid_t pid) {
  int status;
  pid_t result;
  do { result = waitpid(pid, &status, __WALL); } while (result < 0 && errno == EINTR);
  assert(result == pid && WIFSTOPPED(status));
  return status;
}

static char state(pid_t pid, int* tracer) {
  char path[80], line[512], found = '?';
  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  FILE* input = fopen(path, "r");
  assert(input);
  *tracer = -1;
  while (fgets(line, sizeof(line), input)) {
    if (!strncmp(line, "State:\t", 7)) found = line[7];
    if (!strncmp(line, "TracerPid:\t", 11)) *tracer = atoi(line + 11);
  }
  assert(!fclose(input));
  return found;
}

int main(int argc, char** argv) {
  assert(argc == 2 && getuid() == 0);
  setbuf(stdout, NULL);
  alarm(60);  // Fixture deadline, never a product retirement timeout.
  pid_t zygote = atoi(argv[1]);
  assert(zygote > 1);
  assert(!ptrace(PTRACE_SEIZE, zygote, 0, PTRACE_O_TRACEFORK));
  printf("SEIZED %d\n", zygote);
  int status;
  for (;;) {
    status = waited(zygote);
    if ((unsigned int)status >> 16 == PTRACE_EVENT_FORK) break;
    assert(!ptrace(PTRACE_CONT, zygote, 0, WSTOPSIG(status)));
  }
  unsigned long event = 0;
  assert(!ptrace(PTRACE_GETEVENTMSG, zygote, 0, &event));
  pid_t newborn = (pid_t)event;
  assert(newborn > 1);
  // The parent remains held at FORK. The child has not returned to user code,
  // and therefore cannot have connected/sent its lifetime registration.
  status = waited(newborn);
  assert((unsigned int)status >> 16 == PTRACE_EVENT_STOP && WSTOPSIG(status) == SIGTRAP);
  int handle = (int)syscall(SYS_pidfd_open, newborn, 0);
  assert(handle >= 0);
  // DETACH's signal argument is reliable only at signal-delivery-stop. Queue
  // SIGSTOP while still at the birth stop, then run only to that delivery.
  assert(!syscall(SYS_pidfd_send_signal, handle, SIGSTOP, NULL, 0));
  assert(!ptrace(PTRACE_CONT, newborn, 0, 0));
  status = waited(newborn);
  assert((unsigned int)status >> 16 == 0 && WSTOPSIG(status) == SIGSTOP);
  siginfo_t info;
  assert(!ptrace(PTRACE_GETSIGINFO, newborn, 0, &info) && info.si_signo == SIGSTOP);
  assert(!ptrace(PTRACE_DETACH, newborn, 0, SIGSTOP));
  int tracer;
  char observed;
  do {
    observed = state(newborn, &tracer);
    if (observed != 'T' || tracer) usleep(1000);
  } while (observed != 'T' || tracer);
  assert(!ptrace(PTRACE_DETACH, zygote, 0, 0));
  printf("STOPPED_UNREGISTERED %d PARENT %d STATE %c TRACER %d\n",
         newborn, zygote, observed, tracer);
  // Keep the birth-pinned identity, not a later /proc lookup, across init loss.
  struct pollfd exit_event = {handle, POLLIN, 0};
  int result;
  do { result = poll(&exit_event, 1, 30000); } while (result < 0 && errno == EINTR);
  assert(result == 1 && (exit_event.revents & POLLIN) &&
         !(exit_event.revents & (POLLERR | POLLNVAL)));
  printf("ORIGINAL_NEWBORN_EXITED %d\n", newborn);
  assert(!close(handle));
  return 0;
}
