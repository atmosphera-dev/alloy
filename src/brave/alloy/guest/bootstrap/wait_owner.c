// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/process/alloy_guest_runtime.h"
#include "brave/alloy/guest/linux_lock/pinned_spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int send_status(unsigned int phase, int error, int status, int handle,
                       unsigned int operation) {
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
  return sent == sizeof(packet) ? 0 : sent < 0 ? errno : EPROTO;
}

static int receive_grant(char expected) {
  struct pollfd events[] = {{ALLOY_GUEST_WAIT_STATUS_FD, POLLIN, 0},
                            {ALLOY_GUEST_PARENT_FD, POLLIN, 0}};
  int observed;
  do { observed = poll(events, 2, -1); } while (observed < 0 && errno == EINTR);
  if (observed < 0) return errno;
  if (events[1].revents) return ECANCELED;
  if (events[0].revents & POLLIN) {
    char byte;
    ssize_t count;
    do { count = recv(ALLOY_GUEST_WAIT_STATUS_FD, &byte, 1, MSG_TRUNC | MSG_DONTWAIT); }
    while (count < 0 && errno == EINTR);
    return count == 1 && byte == expected ? 0 : count == 0 ? ECANCELED : EPROTO;
  }
  return ECANCELED;
}

static int receive_signal_result(void) {
  struct pollfd events[] = {{ALLOY_GUEST_WAIT_STATUS_FD, POLLIN, 0},
                            {ALLOY_GUEST_PARENT_FD, POLLIN, 0}};
  int observed;
  do { observed = poll(events, 2, -1); } while (observed < 0 && errno == EINTR);
  if (observed < 0) return errno;
  if (events[1].revents) return ECANCELED;
  int error = 0;
  ssize_t received;
  do { received = recv(ALLOY_GUEST_WAIT_STATUS_FD, &error, sizeof(error), MSG_TRUNC | MSG_DONTWAIT); }
  while (received < 0 && errno == EINTR);
  if (received == 0) return ECANCELED;
  return received == sizeof(error) && error >= 0 ? error : EPROTO;
}

static int spawn_owned(const char* bootstrap, const char* role,
                        const struct alloy_lock_fd_map* maps, size_t count,
                        int gated, struct alloy_pinned_child* child) {
  char* arguments[] = {(char*)bootstrap, (char*)role,
                       gated ? ALLOY_GUEST_WAIT_GATE_ARGUMENT : NULL, NULL};
  int error = alloy_spawn_pinned(bootstrap, arguments, environ, maps, count, 0, child);
  if (!error) error = alloy_pinned_exec_result(child->exec_error, NULL);
  if (child->exec_error >= 0) { close(child->exec_error); child->exec_error = -1; }
  return error;
}

static int retire_child(struct alloy_pinned_child* child, int* status) {
  if (child->pidfd < 0) return 0;
  int error = alloy_pinned_signal(child->pidfd, SIGKILL);
  // A refused signal cannot make a stopped child progress. Return to init's
  // exit, whose kernel sweep owns all remaining enclosure members.
  int waited = error ? 0 : alloy_pinned_wait(child->pidfd, status);
  close(child->pidfd);
  child->pidfd = -1;
  return error ? error : waited;
}

static int probe_teardown(const char* bootstrap, int* status, unsigned int* operation) {
  *operation = ALLOY_GUEST_WAIT_SETUP;
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return errno;
  const struct alloy_lock_fd_map map = {pair[1], ALLOY_GUEST_CONTROL_FD};
  struct alloy_pinned_child probe;
  int error = spawn_owned(bootstrap, ALLOY_GUEST_TEARDOWN_PROBE_ARGUMENT, &map, 1, 0, &probe);
  close(pair[1]);
  if (!error) {
    // Actual applicable wait authority, before admitting any engine process.
    siginfo_t info = {0};
    int result;
    do { result = waitid((idtype_t)3, probe.pidfd, &info, WEXITED | WNOHANG); }
    while (result < 0 && errno == EINTR);
    if (result < 0) { error = errno; *operation = ALLOY_GUEST_WAIT_REAP; }
    else if (info.si_pid) error = EPROTO;
  }
  int ready = 0;
  while (!error && !ready) {
    struct pollfd events[] = {{pair[0], POLLIN, 0}, {probe.pidfd, POLLIN, 0},
        {ALLOY_GUEST_WAIT_STATUS_FD, POLLIN, 0}, {ALLOY_GUEST_PARENT_FD, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 4, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0) { error = errno; break; }
    if (events[2].revents || events[3].revents) {
      error = ECANCELED; *operation = ALLOY_GUEST_WAIT_CANCEL; break;
    }
    if (events[0].revents & POLLIN) {
      char byte;
      ssize_t count;
      do { count = recv(pair[0], &byte, 1, MSG_TRUNC); } while (count < 0 && errno == EINTR);
      if (count == 1 && byte == 'P') ready = 1;
      else error = EPROTO;
    } else if (events[0].revents || events[1].revents) error = EPROTO;
  }
  if (!error) {
    *operation = ALLOY_GUEST_WAIT_SIGNAL;
    // The actual sender is the app, not an exec-transitioned bootstrap label.
    // Init still privately owns and verifies the probe's original wait status.
    error = send_status(ALLOY_GUEST_WAIT_PROBE, 0, 0, probe.pidfd, *operation);
    if (!error) error = receive_signal_result();
  }
  int signal_error = 0;
  if (error && probe.pidfd >= 0) {
    // Error cleanup only; this is never accepted as the app's authority proof.
    signal_error = alloy_pinned_signal(probe.pidfd, SIGKILL);
  }
  // Cooperative EOF is only a fallback for this trusted pre-engine probe when
  // the kernel denies SIGKILL. It is not a runtime retirement mechanism.
  shutdown(pair[0], SHUT_RDWR);
  close(pair[0]);
  if (probe.pidfd >= 0) {
    // Never wait for cooperative EOF if even init cannot kill the probe.
    // Returning the original refusal lets init exit and perform its sweep.
    int waited = signal_error ? 0 : alloy_pinned_wait(probe.pidfd, status);
    if (waited && !error) { error = waited; *operation = ALLOY_GUEST_WAIT_REAP; }
    close(probe.pidfd);
  }
  if (!error && (!WIFSIGNALED(*status) || WTERMSIG(*status) != SIGKILL)) error = EPROTO;
  return error;
}

static int reap_orphans(pid_t preserved) {
  // Nonblocking only. Init's final exit performs the kernel kill-and-reap sweep
  // of all remaining namespace members, including stopped/unregistered forks.
  for (;;) {
    siginfo_t info = {0};
    if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT)) {
      if (errno == EINTR) continue;
      return errno == ECHILD ? 0 : errno;
    }
    if (!info.si_pid) return 0;
    if (info.si_pid == preserved) return 0;
    // Init is the sole waiter. WNOWAIT keeps this direct exited child's PID
    // allocated until its exact handle has consumed the status. Never touch
    // the separately owned supervisor status while reaping adopted children.
    int handle = (int)syscall(SYS_pidfd_open, info.si_pid, 0);
    if (handle < 0) return errno;
    int status;
    int error = alloy_pinned_wait(handle, &status);
    if (close(handle) && !error) error = errno;
    if (error) return error;
  }
}

int alloy_guest_wait_owner(const char* bootstrap) {
  // The only executable entry permitted to own the enclosure is its PID 1.
  if (getpid() != 1 || fcntl(ALLOY_GUEST_WAIT_STATUS_FD, F_SETFD, FD_CLOEXEC) ||
      fcntl(ALLOY_GUEST_PARENT_FD, F_SETFD, FD_CLOEXEC)) return 125;
  int error = receive_grant('G');
  int status = -1;
  unsigned int operation = ALLOY_GUEST_WAIT_SETUP;
  if (!error) error = probe_teardown(bootstrap, &status, &operation);
  if (error) {
    send_status(ALLOY_GUEST_WAIT_READY, error, status, -1, operation);
    send_status(ALLOY_GUEST_WAIT_FINISHED, 0, 0, -1, ALLOY_GUEST_WAIT_SETUP);
    return error == ECANCELED ? 0 : 125;
  }
  int gate[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, gate)) {
    send_status(ALLOY_GUEST_WAIT_READY, errno, status, -1, ALLOY_GUEST_WAIT_START);
    return 125;
  }
  const struct alloy_lock_fd_map maps[] = {
      {ALLOY_GUEST_CONTROL_FD, ALLOY_GUEST_CONTROL_FD},
      {ALLOY_GUEST_RESOURCE_FD, ALLOY_GUEST_RESOURCE_FD},
      {gate[1], ALLOY_GUEST_WAIT_STATUS_FD}};
  struct alloy_pinned_child supervisor;
  error = spawn_owned(bootstrap, "--alloy-supervisor", maps, 3, 1, &supervisor);
  close(gate[1]);
  close(ALLOY_GUEST_CONTROL_FD);
  close(ALLOY_GUEST_RESOURCE_FD);
  int announced = send_status(ALLOY_GUEST_WAIT_READY, error, status,
                               error ? -1 : supervisor.pidfd, ALLOY_GUEST_WAIT_START);
  int cancelled = 0;
  if (!error && !announced) {
    error = receive_grant('A');
    cancelled = error == ECANCELED;
    if (cancelled) error = 0;
    if (!error && !cancelled) {
      ssize_t sent;
      do { sent = send(gate[0], "G", 1, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
      if (sent != 1) error = sent < 0 ? errno : EPROTO;
    }
  } else if (!error) error = announced;
  close(gate[0]);
  int status_open = 1;
  while (!error && !cancelled) {
    error = reap_orphans(supervisor.process);
    if (error) break;
    struct pollfd events[] = {{supervisor.pidfd, POLLIN, 0},
        {status_open ? ALLOY_GUEST_WAIT_STATUS_FD : -1, POLLIN, 0},
        {ALLOY_GUEST_PARENT_FD, POLLIN, 0}};
    int observed;
    // Match the inner supervisor's periodic reap cadence. This is not a
    // retirement deadline: live children retain custody for any duration.
    do { observed = poll(events, 3, 100); } while (observed < 0 && errno == EINTR);
    if (observed < 0) { error = errno; break; }
    if (events[0].revents & (POLLERR | POLLNVAL)) { error = EIO; break; }
    if (events[0].revents & POLLIN) break;
    if (events[2].revents) {
      // App death ends the enclosure even if the supervisor cannot run. Do not
      // wait on a cooperative child or a later denied signal before init exits.
      return 125;
    }
    if (events[1].revents & (POLLERR | POLLNVAL)) { error = EIO; break; }
    if (events[1].revents) {
      char byte;
      ssize_t count;
      do { count = recv(ALLOY_GUEST_WAIT_STATUS_FD, &byte, 1, MSG_TRUNC | MSG_DONTWAIT); }
      while (count < 0 && errno == EINTR);
      if (count == 0) status_open = 0;
      else if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        error = count < 0 ? errno : EPROTO;
        break;
      }
      // After A, app EOF is not startup cancellation and cannot erase S's
      // outcome. Control EOF drives graceful retirement; Terminate kills init
      // through the app mirror. Keep waiting for the exact inner status.
    }
  }
  int retired = retire_child(&supervisor, &status);
  if (!error) error = retired;
  int swept = reap_orphans(-1);
  if (!error) error = swept;
  if (cancelled && !error) status = 0;
  send_status(ALLOY_GUEST_WAIT_FINISHED, error, status, -1, ALLOY_GUEST_WAIT_SETUP);
  // This exit is the authoritative kernel sweep. The app must observe the
  // original init pidfd exit, not just FINISHED, before publishing CLOSED.
  return !error && status == 0 ? 0 : 125;
}
