// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/process/alloy_guest_runtime.h"
#include "brave/alloy/guest/protocol/wire.h"
#include "brave/alloy/guest/linux_lock/protection.h"
#include "brave/alloy/guest/linux_lock/child_launch.h"
#include "brave/alloy/guest/linux_lock/pinned_spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

struct member {
  int pidfd;
  int liveness;
  pid_t process;
};

struct members {
  struct member* items;
  size_t count;
};

static int prune_members(struct members* members) {
  for (size_t i = 0; i < members->count;) {
    int exited = alloy_pinned_exited(members->items[i].pidfd, 0);
    if (exited < 0) return 0;
    if (!exited) { ++i; continue; }
    close(members->items[i].pidfd);
    if (members->items[i].liveness >= 0) close(members->items[i].liveness);
    members->items[i] = members->items[--members->count];
  }
  return 1;
}

static int reap(void) {
  for (;;) {
    siginfo_t info = {0};
    if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG)) {
      if (errno == EINTR) continue;
      return errno == ECHILD ? 0 : errno;
    }
    if (!info.si_pid) return 0;
  }
}

static int retire(struct members* members) {
  int error = 0;
  for (size_t i = 0; i < members->count; ++i) {
    struct member* member = &members->items[i];
    if (member->liveness >= 0) {
      close(member->liveness);
      member->liveness = -1;
    }
    if (member->pidfd >= 0) {
      int signaled = alloy_pinned_signal(member->pidfd, SIGKILL);
      if (signaled && !error) error = signaled;
    }
  }
  // Owned children and orphans reparent to this subreaper. Never report final
  // completion while an admitted pidfd is live, even if its parent is reaping.
  for (size_t i = 0; i < members->count; ++i) {
    struct member* member = &members->items[i];
    if (member->pidfd >= 0) {
      if (alloy_pinned_exited(member->pidfd, 1) != 1 && !error) error = EIO;
      close(member->pidfd);
    }
  }
  int reaped = reap();
  if (!error) error = reaped;
  // Live, unregistered orphans belong to enclosure init. Its kernel exit
  // sweeps them even when stopped; never block here on waitpid(-1).
  free(members->items);
  members->items = NULL;
  members->count = 0;
  return error;
}

static int admit(int registration, struct members* members) {
  uint32_t packet[2] = {0, 0};
  struct iovec io = {packet, sizeof(packet)};
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(253 * sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  struct msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  ssize_t received;
  do { received = recvmsg(registration, &message, MSG_CMSG_CLOEXEC); }
  while (received < 0 && errno == EINTR);
  struct ucred credential = {0, 0, 0};
  size_t credential_count = 0;
  size_t handle_count = 0;
  int endpoint = -1;
  int valid = received == sizeof(packet) &&
              !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
              packet[0] == ALLOY_GUEST_REGISTRATION_MAGIC && packet[1] <= 1;
  for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
        header->cmsg_len == CMSG_LEN(sizeof(credential))) {
      memcpy(&credential, CMSG_DATA(header), sizeof(credential));
      ++credential_count;
    } else if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
      size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      int* handles = (int*)CMSG_DATA(header);
      for (size_t i = 0; i < count; ++i) {
        if (handle_count++ == 0)
          endpoint = handles[i];
        else
          close(handles[i]);
      }
    } else {
      valid = 0;
    }
  }
  int type = 0;
  socklen_t type_size = sizeof(type);
  struct ucred peer = {0, 0, 0};
  socklen_t peer_size = sizeof(peer);
  valid = valid && credential_count == 1 && handle_count == 1 &&
          credential.uid == getuid() && credential.pid > 1 &&
          !getsockopt(endpoint, SOL_SOCKET, SO_TYPE, &type, &type_size) &&
          type == SOCK_SEQPACKET &&
          !getsockopt(endpoint, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) &&
          peer.pid == credential.pid && peer.uid == credential.uid;
  int pidfd = valid ? (int)syscall(SYS_pidfd_open, credential.pid, 0) : -1;
  if (valid && pidfd < 0 && errno == ESRCH) {
    close(endpoint);  // The original child exited before admission.
    return 1;
  }
  if (pidfd >= 0) {
    // Pin first, then prove this is still the sole live peer. The registering
    // entry cannot fork or exec before A. A cached credential PID alone is not
    // authority after its original peer closed and the PID was recycled.
    struct pollfd alive = {endpoint, POLLIN, 0};
    int observed;
    do { observed = poll(&alive, 1, 0); } while (observed < 0 && errno == EINTR);
    if (observed < 0 || alive.revents) {
      close(pidfd);
      close(endpoint);
      return observed < 0 ? 0 : 1;
    }
  }
  if (pidfd >= 0) {
    // A previously admitted process may already have exited and been reaped by
    // its native parent. Its numeric PID must not veto a new pinned identity.
    if (!prune_members(members)) {
      alloy_pinned_signal(pidfd, SIGKILL);
      alloy_pinned_exited(pidfd, 1);
      close(pidfd);
      close(endpoint);
      return 0;
    }
    for (size_t i = 0; i < members->count; ++i) {
      if (members->items[i].process == credential.pid) {
        // A duplicate live registration is invalid. Reused numeric PIDs never
        // replace the old pidfd identity; retire exited entries in the loop.
        close(pidfd);
        pidfd = -1;
        break;
      }
    }
  }
  struct member* replacement = NULL;
  if (pidfd >= 0 && members->count < SIZE_MAX / sizeof(struct member))
    replacement = realloc(members->items,
                          (members->count + 1) * sizeof(struct member));
  if (!replacement) {
    if (pidfd >= 0) {
      // Custody is not released just because the registry allocation failed.
      alloy_pinned_signal(pidfd, SIGKILL);
      alloy_pinned_exited(pidfd, 1);
      close(pidfd);
    }
    if (endpoint >= 0)
      close(endpoint);
    return 0;
  }
  members->items = replacement;
  members->items[members->count++] =
      (struct member){pidfd, endpoint, credential.pid};
  ssize_t acknowledged;
  do {
    acknowledged = send(endpoint, "A", 1, MSG_NOSIGNAL);
  } while (acknowledged < 0 && errno == EINTR);
  if (acknowledged == 1)
    return 1;
  if ((errno == EPIPE || errno == ECONNRESET) ||
      alloy_pinned_exited(pidfd, 0) == 1) {
    // Keep the exact pidfd until exit even if its endpoint closed early. No
    // admission ack reached this entry, but it must not become untracked.
    close(endpoint);
    members->items[members->count - 1].liveness = -1;
    return 1;
  }
  return 0;
}

struct forwarding {
  unsigned char packet[ALLOY_GUEST_CONTROL_CHUNK_SIZE];
  size_t count;
  size_t remaining;
};

static int receive_forward(int from, struct forwarding* forward, int* closing) {
  // One bounded packet at a time, with backpressure independent of logical
  // control length. Never block child supervision on a peer's receive queue.
  unsigned char* packet = forward->packet;
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(253 * sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  struct iovec io = {packet, sizeof(forward->packet)};
  struct msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  ssize_t count;
  do { count = recvmsg(from, &message, MSG_CMSG_CLOEXEC); }
  while (count < 0 && errno == EINTR);
  int valid = count > 0 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
  for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header;
       header = CMSG_NXTHDR(&message, header)) {
    valid = 0;
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
      size_t size = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      int* handles = (int*)CMSG_DATA(header);
      for (size_t i = 0; i < size; ++i)
        close(handles[i]);
    }
  }
  if (!valid)
    return 0;
  if (!forward->remaining) {
    if ((size_t)count < ALLOY_GUEST_ENVELOPE_SIZE ||
        memcmp(packet, "AGS1", 4) || packet[4] ||
        packet[5] != ALLOY_GUEST_PROTOCOL_VERSION ||
        packet[12] || packet[13] || packet[14] || packet[15])
      return 0;
    size_t payload = 0;
    for (size_t i = 8; i < 12; ++i)
      payload = (payload << 8) | packet[i];
    if (payload > ALLOY_GUEST_MAX_CONTROL_PAYLOAD)
      return 0;
    forward->remaining = ALLOY_GUEST_ENVELOPE_SIZE + payload;
    unsigned int kind = (unsigned int)packet[6] * 256 + packet[7];
    if (kind == ALLOY_GUEST_CLOSE_MESSAGE || kind == ALLOY_GUEST_CLOSED_MESSAGE ||
        kind == ALLOY_GUEST_FAILURE_MESSAGE)
      *closing = 1;
  }
  if ((size_t)count > forward->remaining)
    return 0;
  forward->remaining -= (size_t)count;
  forward->count = (size_t)count;
  return 1;
}

static int flush_forward(int to, struct forwarding* forward) {
  if (!forward->count)
    return 1;
  ssize_t sent = send(to, forward->packet, forward->count,
                      MSG_NOSIGNAL | MSG_DONTWAIT);
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return 1;
  if (sent != (ssize_t)forward->count)
    return 0;
  forward->count = 0;
  return 1;
}

static int supervise_prepared(const char* bootstrap) {
  int registration[2], browser_control[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, registration) ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, browser_control))
    return 125;
  int enabled = 1;
  if (setsockopt(registration[0], SOL_SOCKET, SO_PASSCRED,
                 &enabled, sizeof(enabled)))
    return 125;
  const struct alloy_lock_fd_map maps[] = {
      {registration[1], ALLOY_GUEST_REGISTRATION_FD},
      {browser_control[1], ALLOY_GUEST_CONTROL_FD},
      {ALLOY_GUEST_RESOURCE_FD, ALLOY_GUEST_RESOURCE_FD}};
  char* arguments[] = {(char*)bootstrap, NULL};
  struct alloy_pinned_child browser;
  int launched = alloy_spawn_pinned(bootstrap, arguments, environ, maps, 3, 0, &browser);
  if (!launched) launched = alloy_pinned_exec_result(browser.exec_error, NULL);
  if (browser.exec_error >= 0) close(browser.exec_error);
  close(registration[1]);
  close(browser_control[1]);
  close(ALLOY_GUEST_RESOURCE_FD);
  if (launched) {
    if (browser.pidfd >= 0) {
      alloy_pinned_signal(browser.pidfd, SIGKILL);
      int status;
      alloy_pinned_wait(browser.pidfd, &status);
      close(browser.pidfd);
    }
    return 125;
  }
  int browser_pidfd = browser.pidfd;
  struct members members = {NULL, 0};
  struct forwarding to_browser = {{0}, 0, 0};
  struct forwarding to_app = {{0}, 0, 0};
  int closing = 0;
  int browser_dead = 0;
  int browser_eof = 0;
  int result = 125;
  for (;;) {
    if (reap()) break;
    if (!alloy_lock_scratch_valid())
      break;
    struct pollfd events[] = {
        {ALLOY_GUEST_CONTROL_FD, (short)((to_browser.count ? 0 : POLLIN) |
                                         (to_app.count ? POLLOUT : 0)), 0},
        {(browser_dead && to_app.count) || browser_eof ? -1 : browser_control[0],
                                (short)((to_app.count ? 0 : POLLIN) |
                                      (to_browser.count ? POLLOUT : 0)), 0},
        {registration[0], POLLIN, 0},
        {browser_dead ? -1 : browser_pidfd, POLLIN, 0}};
    int observed = poll(events, 4, 100);
    if (observed < 0 && errno == EINTR)
      continue;
    if (observed < 0)
      break;
    // App close/death is processed before registration in this same sequence.
    // A later entry cannot receive its admission ack once closing begins.
    if (events[0].revents & POLLHUP) {
      result = 0;
      break;
    }
    if (events[0].revents & (POLLERR | POLLNVAL)) break;
    if ((events[0].revents & POLLIN) &&
        !receive_forward(ALLOY_GUEST_CONTROL_FD, &to_browser, &closing))
      break;
    if ((events[1].revents & POLLIN) && !to_app.count) {
      char byte;
      ssize_t pending = recv(browser_control[0], &byte, 1, MSG_PEEK | MSG_DONTWAIT);
      if (pending == 0)
        browser_eof = 1;
      else if (!receive_forward(browser_control[0], &to_app, &closing))
        break;
    }
    if ((events[0].revents & POLLOUT) &&
        !flush_forward(ALLOY_GUEST_CONTROL_FD, &to_app))
      break;
    if ((events[1].revents & POLLOUT) &&
        !flush_forward(browser_control[0], &to_browser))
      break;
    if (events[3].revents || (events[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
      browser_dead = 1;
      closing = 1;
      if (!to_app.count) {
        char byte;
        browser_eof = recv(browser_control[0], &byte, 1,
                            MSG_PEEK | MSG_DONTWAIT) == 0;
      }
    }
    if (closing && registration[0] >= 0) {
      close(registration[0]);
      registration[0] = -1;
    }
    if (registration[0] >= 0 && events[2].revents && !admit(registration[0], &members))
      break;
    if (browser_dead && browser_eof && !to_app.count) {
      result = 0;
      break;
    }
    if (!prune_members(&members)) break;
    if (reap()) break;
  }
  // Closing the registration receiver first refuses even entries already
  // queued in the kernel. They are still in the pre-engine admission handshake.
  if (registration[0] >= 0)
    close(registration[0]);
  close(browser_control[0]);
  if (alloy_pinned_signal(browser_pidfd, SIGKILL)) result = 125;
  if (retire(&members)) result = 125;
  if (alloy_pinned_exited(browser_pidfd, 1) != 1) result = 125;
  close(browser_pidfd);
  if (reap()) result = 125;
  close(ALLOY_GUEST_CONTROL_FD);
  return result;
}

int alloy_guest_supervise(const char* bootstrap) {
  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0)) return 125;
  int result = alloy_lock_prepare_scratch() ? supervise_prepared(bootstrap) : 125;
  // One owner cleans all preparation and launch failures, not just the normal
  // loop exit. The remover checks the descriptor's exact current RAM identity.
  alloy_lock_remove_scratch();
  return result;
}
