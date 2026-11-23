// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test entry around the real supervisor/wait-owner and runtime registration.
#define _GNU_SOURCE
#include "base/process/alloy_guest_runtime.h"
#include <assert.h>
#include <errno.h>
#include <linux/sched.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int alloy_guest_supervise(const char* bootstrap);
#ifdef ALLOY_LIFETIME_WAIT_OWNER
int alloy_guest_wait_owner(const char* bootstrap);
#endif

// Scratch enforcement belongs to separate real CEF/lock gates. These stubs
// isolate process custody while linking the actual supervisor implementation.
int alloy_lock_prepare_scratch(void) { return 1; }
int alloy_lock_scratch_valid(void) { return 1; }
void alloy_lock_remove_scratch(void) {}

static int fail_poll, fail_recv, fail_wait;
#ifdef ALLOY_LIFETIME_PINNED_SPAWN
static int deny_kill;
int __real_alloy_pinned_signal(int, int);
int __wrap_alloy_pinned_signal(int fd, int number) {
  return deny_kill && number == SIGKILL ? EPERM : __real_alloy_pinned_signal(fd, number);
}
#endif
static int injected_error = EIO;
static int fault(int* selected) {
  if (!*selected) return 0;
  *selected = 0;
  assert(write(203, "E", 1) == 1);
  errno = injected_error;
  return 1;
}
int __real_poll(struct pollfd*, nfds_t, int);
int __wrap_poll(struct pollfd* events, nfds_t count, int timeout) {
  return fault(&fail_poll) ? -1 : __real_poll(events, count, timeout);
}
ssize_t __real_recvmsg(int, struct msghdr*, int);
ssize_t __wrap_recvmsg(int fd, struct msghdr* message, int flags) {
  return fault(&fail_recv) ? -1 : __real_recvmsg(fd, message, flags);
}
int __real_waitid(idtype_t, id_t, siginfo_t*, int);
int __wrap_waitid(idtype_t type, id_t id, siginfo_t* info, int flags) {
  return type == P_ALL && fault(&fail_wait) ? -1 : __real_waitid(type, id, info, flags);
}
pid_t __real_waitpid(pid_t, int*, int);
pid_t __wrap_waitpid(pid_t pid, int* status, int flags) {
  return pid == -1 && (flags & WNOHANG) && fault(&fail_wait)
      ? -1 : __real_waitpid(pid, status, flags);
}

int main(int argc, char** argv) {
  char executable[4096];
  ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  assert(size > 0);
  executable[size] = 0;
#ifdef ALLOY_LIFETIME_WAIT_OWNER
  if (argc == 2 && !strcmp(argv[1], ALLOY_GUEST_WAIT_OWNER_ARGUMENT)) {
    deny_kill = strstr(executable, "/kill-refused-") != NULL;
    return alloy_guest_wait_owner(executable);
  }
  if (argc == 2 && !strcmp(argv[1], ALLOY_GUEST_TEARDOWN_PROBE_ARGUMENT)) {
    if (strstr(executable, "/startup-cancel/")) raise(SIGSTOP);
    assert(send(ALLOY_GUEST_CONTROL_FD, "P", 1, MSG_NOSIGNAL) == 1);
    if (strstr(executable, "/kill-refused-probe/")) raise(SIGSTOP);
    char byte;
    while (read(ALLOY_GUEST_CONTROL_FD, &byte, 1) < 0 && errno == EINTR) {}
    return 0;
  }
#endif
  if (argc >= 2 && !strcmp(argv[1], "--alloy-supervisor")) {
    if (argc == 3) {
      char grant;
      ssize_t count;
      do { count = recv(195, &grant, 1, MSG_TRUNC); } while (count < 0 && errno == EINTR);
      close(195);
      if (count == 0) return 0;
      assert(count == 1 && grant == 'G');
    }
    fail_poll = strstr(executable, "/fault-inner-poll/") != NULL;
    fail_recv = strstr(executable, "/fault-inner-recv/") != NULL ||
                strstr(executable, "/eintr-registration/") != NULL;
    fail_wait = strstr(executable, "/fault-inner-wait/") != NULL;
    injected_error = strstr(executable, "/eintr-registration/") ? EINTR : EIO;
    if (strstr(executable, "/orphan-reap/")) {
      // Fixture-only direct init orphan. CLONE_PARENT deliberately bypasses
      // the supervisor's subreaper so its parent is the enclosure init.
      pid_t orphan = (pid_t)syscall(SYS_clone, CLONE_PARENT | SIGCHLD, NULL, NULL, NULL, 0);
      assert(orphan >= 0);
      if (!orphan) {
        assert(!prctl(PR_SET_NAME, "alloy-orphan", 0, 0, 0));
        raise(SIGSTOP);
        _exit(0);
      }
    }
    return alloy_guest_supervise(executable);
  }
  assert(alloy_guest_register_runtime(1));
  assert(!prctl(PR_SET_PDEATHSIG, SIGKILL));
  if (strstr(executable, "/proc-view/")) {
    FILE* status = fopen("/proc/self/status", "r");
    assert(status);
    char line[512];
    int process = -1;
    while (fgets(line, sizeof(line), status)) {
      if (sscanf(line, "Pid:\t%d", &process) == 1) break;
    }
    assert(!fclose(status));
    assert(process == getpid());
    assert(getsid(0) == 1);
  }
  assert(!prctl(PR_SET_NAME, "alloy-lifetime", 0, 0, 0));
  if (strstr(executable, "/registration-reuse/") || strstr(executable, "/registration-allocation/") ||
      strstr(executable, "/registration-allocation-denied/") ||
      strstr(executable, "/registration-duplicate/")) {
    char* name = strrchr(executable, '/');
    assert(name && (size_t)(name - executable) + sizeof("/alloy_registry_probe") <= sizeof(executable));
    strcpy(name + 1, "alloy_registry_probe");
    const char* check = strstr(executable, "/registration-allocation-denied/") ? "allocation-denied" :
                         strstr(executable, "/registration-allocation/") ? "allocation" :
                        strstr(executable, "/registration-duplicate/") ? "duplicate" : "reuse";
    execl("/usr/bin/unshare", "unshare", "-Urpf", "--mount-proc", executable, check, NULL);
    assert(!"Could not execute fixture-only namespace helper");
  }
  if (strstr(executable, "/startup-cancel/")) raise(SIGSTOP);
  if (strstr(executable, "/late/") || strstr(executable, "/inner-lost/") ||
      strstr(executable, "/stopped-creator/") || strstr(executable, "/app-dead/")) {
    pid_t late = fork();
    assert(late >= 0);
    if (!late) {
      // Deliberately never register. Confirmed stopped by the external harness.
      close(ALLOY_GUEST_CONTROL_FD);
      close(ALLOY_GUEST_RESOURCE_FD);
      raise(SIGSTOP);
      for (;;) pause();
    }
  }
  for (;;) pause();
}
