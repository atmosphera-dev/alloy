// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/process/alloy_guest_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pid_t initialized_process;
static int owns_browser_control;

void alloy_guest_close_fork_control(void) {
  if (owns_browser_control) {
    close(ALLOY_GUEST_CONTROL_FD);
    close(ALLOY_GUEST_RESOURCE_FD);
    owns_browser_control = 0;
  }
}

static void supervisor_lost(int signal_number, siginfo_t* info, void* context) {
  (void)signal_number;
  (void)info;
  (void)context;
  // Only the supervisor owns the opposite endpoint. It sends no traffic after
  // the admission acknowledgement. EOF or any later traffic ends admission.
  _exit(125);
}

static int arm_liveness(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = supervisor_lost;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  int signal_number = SIGRTMIN + 6;
  // Linux falls back to SIGIO if the realtime queue is full. Handle both, also
  // when this runtime is PID 1 in a Chromium-created namespace.
  if (sigaction(signal_number, &action, NULL) ||
      sigaction(SIGIO, &action, NULL) ||
      fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETSIG, signal_number) ||
      fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETOWN, getpid()) ||
      fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETFL, O_ASYNC | O_NONBLOCK))
    return 0;
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, signal_number);
  sigaddset(&signals, SIGIO);
  if (sigprocmask(SIG_UNBLOCK, &signals, NULL))
    return 0;
  // Close the acknowledgement-to-arm race. A closed peer must not be admitted
  // just because its EOF occurred before O_ASYNC was installed.
  char byte;
  ssize_t result = recv(ALLOY_GUEST_LIVENESS_FD, &byte, 1,
                        MSG_PEEK | MSG_DONTWAIT);
  return result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

int alloy_guest_is_runtime(void) {
  const char* selected = getenv("ALLOY_GUEST_RUNTIME");
  return selected && strcmp(selected, "1") == 0;
}

int alloy_guest_register_runtime(int pre_engine) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
    return 0;
  if (pair[1] == ALLOY_GUEST_LIVENESS_FD) {
    int moved = fcntl(pair[1], F_DUPFD_CLOEXEC, ALLOY_GUEST_FACTS_FD + 1);
    if (moved < 0) {
      close(pair[0]);
      close(pair[1]);
      return 0;
    }
    close(pair[1]);
    pair[1] = moved;
  }
  // Close inherited copies of the parent's liveness endpoint before replacing
  // it. No descendant ever inherits the supervisor's endpoint.
  if (pair[0] != ALLOY_GUEST_LIVENESS_FD) {
    if (dup3(pair[0], ALLOY_GUEST_LIVENESS_FD, O_CLOEXEC) < 0) {
      close(pair[0]);
      close(pair[1]);
      return 0;
    }
    close(pair[0]);
  }
  uint32_t packet[2] = {ALLOY_GUEST_REGISTRATION_MAGIC,
                        pre_engine ? 1u : 0u};
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  struct iovec io = {packet, sizeof(packet)};
  struct msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &pair[1], sizeof(int));
  ssize_t sent;
  do {
    sent = sendmsg(ALLOY_GUEST_REGISTRATION_FD, &message, MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  close(pair[1]);
  if (sent != sizeof(packet))
    return 0;
  char acknowledgement = 0;
  ssize_t received;
  do {
    received = read(ALLOY_GUEST_LIVENESS_FD, &acknowledgement, 1);
  } while (received < 0 && errno == EINTR);
  if (received != 1 || acknowledgement != 'A')
    return 0;
  initialized_process = getpid();
  return arm_liveness();
}

void alloy_guest_initialize_runtime(void) {
  if (!alloy_guest_is_runtime())
    return;
  pid_t process = getpid();
  if (initialized_process == process)
    return;
  if (initialized_process != 0) {
    // A real fork, including namespace/zygote forks, cannot reuse the parent's
    // pidfd registration or its async-signal owner.
    if (!alloy_guest_register_runtime(0))
      _exit(125);
    return;
  }
  struct alloy_guest_bootstrap_facts facts;
  int seals = fcntl(ALLOY_GUEST_FACTS_FD, F_GET_SEALS);
  int required = F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
  ssize_t count = pread(ALLOY_GUEST_FACTS_FD, &facts, sizeof(facts), 0);
  if (count != sizeof(facts) || seals < 0 || (seals & required) != required ||
      facts.magic != ALLOY_GUEST_FACTS_MAGIC || facts.process != process ||
      !facts.before_engine || !facts.handles_closed || !arm_liveness())
    _exit(125);
  initialized_process = process;
  owns_browser_control = facts.owns_browser_control != 0;
  if (pthread_atfork(NULL, NULL, alloy_guest_close_fork_control))
    _exit(125);
  if (fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETFD, FD_CLOEXEC) ||
      fcntl(ALLOY_GUEST_REGISTRATION_FD, F_SETFD, FD_CLOEXEC))
    _exit(125);
}
