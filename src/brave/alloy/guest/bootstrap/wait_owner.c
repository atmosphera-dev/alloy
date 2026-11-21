// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/process/alloy_guest_runtime.h"
#include "brave/alloy/guest/linux_lock/child_launch.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int send_status(unsigned int phase, int error, int status, int handle, unsigned int operation) {
  const struct alloy_guest_wait_status packet = {
      ALLOY_GUEST_WAIT_STATUS_MAGIC, phase, error, status, operation};
  struct iovec io = {(void*)&packet, sizeof(packet)};
  union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control;
  memset(&control, 0, sizeof(control));
  struct msghdr message = {0};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  if (handle >= 0) {
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &handle, sizeof(handle));
  }
  ssize_t sent;
  do { sent = sendmsg(ALLOY_GUEST_WAIT_STATUS_FD, &message, MSG_NOSIGNAL); }
  while (sent < 0 && errno == EINTR);
  return sent == sizeof(packet);
}

static int spawn_owned(const char* bootstrap, const char* role,
                       const struct alloy_lock_fd_map* maps, size_t count,
                       int gated, pid_t* process) {
  posix_spawn_file_actions_t actions;
  int copies[3];
  if (count > 3 || !alloy_lock_spawn_actions(&actions, maps, count, copies))
    return 0;
  char* arguments[] = {(char*)bootstrap, (char*)role,
                      gated ? ALLOY_GUEST_WAIT_GATE_ARGUMENT : NULL, NULL};
  int error = posix_spawn(process, bootstrap, &actions, NULL, arguments, environ);
  alloy_lock_spawn_actions_destroy(&actions, count, copies);
  if (error) errno = error;
  return error == 0;
}

static int wait_original(pid_t process) {
  int status;
  pid_t reaped;
  do { reaped = waitpid(process, &status, 0); } while (reaped < 0 && errno == EINTR);
  return reaped == process ? status : -1;
}

static int kill_original(int handle) {
  int result;
  do { result = (int)syscall(SYS_pidfd_send_signal, handle, SIGKILL, NULL, 0); }
  while (result < 0 && errno == EINTR);
  return result;
}

static int probe_teardown(const char* bootstrap, int* status, unsigned int* operation) {
  *operation = ALLOY_GUEST_WAIT_SETUP;
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return errno;
  const struct alloy_lock_fd_map map = {pair[1], ALLOY_GUEST_CONTROL_FD};
  pid_t probe = -1;
  if (!spawn_owned(bootstrap, ALLOY_GUEST_TEARDOWN_PROBE_ARGUMENT, &map, 1, 0, &probe)) {
    int error = errno;
    close(pair[0]); close(pair[1]);
    return error;
  }
  close(pair[1]);
  int handle = (int)syscall(SYS_pidfd_open, probe, 0);
  int error = handle < 0 ? errno : 0;
  if (handle < 0) *operation = ALLOY_GUEST_WAIT_PIDFD;
  int ready = 0;
  while (!error && !ready) {
    struct pollfd events[] = {{pair[0], POLLIN, 0}, {handle, POLLIN, 0},
                             {ALLOY_GUEST_WAIT_STATUS_FD, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 3, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0) { error = errno; break; }
    // Cancellation wins over readiness in this same sequence.
    if (events[2].revents) { error = ECANCELED; *operation = ALLOY_GUEST_WAIT_CANCEL; break; }
    if (events[0].revents & POLLIN) {
      char byte;
      ssize_t count;
      do { count = recv(pair[0], &byte, 1, MSG_TRUNC); } while (count < 0 && errno == EINTR);
      if (count == 1 && byte == 'P') ready = 1;
      else error = EPROTO;
    } else if (events[0].revents || events[1].revents) {
      error = EPROTO;
    }
  }
  // The owner can still reach a stopped probe when readiness is cancelled.
  // EOF is only a trusted pre-admission escape if actual SIGKILL is denied.
  if (handle >= 0 && kill_original(handle) < 0 && !error) {
    error = errno;
    *operation = ALLOY_GUEST_WAIT_SIGNAL;
  }
  close(pair[0]);
  *status = wait_original(probe);
  if (handle >= 0) close(handle);
  if (!error && (*status < 0 || !WIFSIGNALED(*status) || WTERMSIG(*status) != SIGKILL))
    error = EPROTO;
  return error;
}

int alloy_guest_wait_owner(const char* bootstrap) {
  // Only this new static process changes signal handling, never its embedder.
  struct sigaction action = {0};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGCHLD);
  if (sigaction(SIGCHLD, &action, NULL) ||
      sigprocmask(SIG_UNBLOCK, &signals, NULL) ||
      prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) ||
      fcntl(ALLOY_GUEST_WAIT_STATUS_FD, F_SETFD, FD_CLOEXEC)) return 125;
  // No child is launched before the app pins this owner's original identity.
  char grant;
  ssize_t received;
  do { received = recv(ALLOY_GUEST_WAIT_STATUS_FD, &grant, 1, MSG_TRUNC); }
  while (received < 0 && errno == EINTR);
  int status = -1;
  unsigned int operation = ALLOY_GUEST_WAIT_CANCEL;
  int error = received == 1 && grant == 'G' ? probe_teardown(bootstrap, &status, &operation) : ECANCELED;
  if (error) {
    send_status(ALLOY_GUEST_WAIT_READY, error, status, -1, operation);
    send_status(ALLOY_GUEST_WAIT_FINISHED, 0, 0, -1, ALLOY_GUEST_WAIT_SETUP);
    return 125;
  }
  // Transfer its original exit barrier before allowing any runtime admission.
  // If this outer owner dies before the transfer/ack, gate EOF releases only
  // the trusted static inner entry. After ack, the app also owns its pidfd.
  int gate[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, gate)) {
    send_status(ALLOY_GUEST_WAIT_READY, errno, status, -1, ALLOY_GUEST_WAIT_START);
    send_status(ALLOY_GUEST_WAIT_FINISHED, 0, -1, -1, ALLOY_GUEST_WAIT_SETUP);
    return 125;
  }
  const struct alloy_lock_fd_map maps[] = {
      {ALLOY_GUEST_CONTROL_FD, ALLOY_GUEST_CONTROL_FD},
      {ALLOY_GUEST_RESOURCE_FD, ALLOY_GUEST_RESOURCE_FD},
      {gate[1], ALLOY_GUEST_WAIT_STATUS_FD}};
  pid_t supervisor = -1;
  int launched = spawn_owned(bootstrap, "--alloy-supervisor", maps, 3, 1, &supervisor);
  int handle = launched ? (int)syscall(SYS_pidfd_open, supervisor, 0) : -1;
  error = handle >= 0 ? 0 : errno;
  close(gate[1]);
  close(ALLOY_GUEST_CONTROL_FD);
  close(ALLOY_GUEST_RESOURCE_FD);
  int announced = send_status(ALLOY_GUEST_WAIT_READY, error, status, handle, ALLOY_GUEST_WAIT_START);
  int gate_failed = 0;
  if (!error && announced) {
    do { received = recv(ALLOY_GUEST_WAIT_STATUS_FD, &grant, 1, MSG_TRUNC); }
    while (received < 0 && errno == EINTR);
    if (received == 1 && grant == 'A') {
      ssize_t sent;
      do { sent = send(gate[0], "G", 1, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
      gate_failed = sent != 1;
    }
  }
  close(gate[0]);
  status = launched ? wait_original(supervisor) : -1;
  if (gate_failed) status = -1;
  if (handle >= 0) close(handle);
  // If the inner subreaper died, its liveness endpoints release the runtime.
  // Retain this outer owner until every adopted original child is reaped too.
  for (;;) {
    pid_t child = waitpid(-1, NULL, 0);
    if (child > 0 || (child < 0 && errno == EINTR)) continue;
    if (child < 0 && errno != ECHILD) status = -1;
    break;
  }
  send_status(ALLOY_GUEST_WAIT_FINISHED, 0, status, -1, ALLOY_GUEST_WAIT_SETUP);
  return status == 0 ? 0 : 125;
}
