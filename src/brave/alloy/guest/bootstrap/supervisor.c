// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/process/alloy_guest_runtime.h"
#include "brave/alloy/guest/protocol/wire.h"

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

static void reap(void) {
  for (;;) {
    pid_t process = waitpid(-1, NULL, WNOHANG);
    if (process > 0 || (process < 0 && errno == EINTR))
      continue;
    break;
  }
}

static void retire(struct members* members) {
  for (size_t i = 0; i < members->count; ++i) {
    struct member* member = &members->items[i];
    if (member->liveness >= 0) {
      close(member->liveness);
      member->liveness = -1;
    }
    if (member->pidfd >= 0)
      syscall(SYS_pidfd_send_signal, member->pidfd, SIGKILL, NULL, 0);
  }
  // Owned children and orphans reparent to this subreaper. Never report final
  // completion while an admitted pidfd is live, even if its parent is reaping.
  for (size_t i = 0; i < members->count; ++i) {
    struct member* member = &members->items[i];
    if (member->pidfd >= 0) {
      struct pollfd event = {member->pidfd, POLLIN, 0};
      int result;
      do {
        result = poll(&event, 1, -1);
      } while (result < 0 && errno == EINTR);
      close(member->pidfd);
    }
  }
  for (;;) {
    pid_t process = waitpid(-1, NULL, 0);
    if (process > 0 || (process < 0 && errno == EINTR))
      continue;
    break;
  }
  free(members->items);
  members->items = NULL;
  members->count = 0;
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
  ssize_t received = recvmsg(registration, &message, MSG_CMSG_CLOEXEC);
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
  if (pidfd >= 0) {
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
    if (pidfd >= 0)
      close(pidfd);
    if (endpoint >= 0)
      close(endpoint);
    return 0;
  }
  members->items = replacement;
  members->items[members->count++] =
      (struct member){pidfd, endpoint, credential.pid};
  return send(endpoint, "A", 1, MSG_NOSIGNAL) == 1;
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
  ssize_t count = recvmsg(from, &message, MSG_CMSG_CLOEXEC);
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

int alloy_guest_supervise(const char* bootstrap) {
  if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
    return 125;
  int registration[2], browser_control[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, registration) ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, browser_control))
    return 125;
  int enabled = 1;
  if (setsockopt(registration[0], SOL_SOCKET, SO_PASSCRED,
                 &enabled, sizeof(enabled)))
    return 125;
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions))
    return 125;
  int setup = posix_spawn_file_actions_adddup2(&actions, registration[1],
                                               ALLOY_GUEST_REGISTRATION_FD) ||
              posix_spawn_file_actions_adddup2(&actions, browser_control[1],
                                               ALLOY_GUEST_CONTROL_FD);
  if (!setup)
    setup = posix_spawn_file_actions_adddup2(&actions, ALLOY_GUEST_RESOURCE_FD,
                                            ALLOY_GUEST_RESOURCE_FD);
  char* arguments[] = {(char*)bootstrap, NULL};
  pid_t browser = -1;
  int launched = setup ? -1 : posix_spawn(&browser, bootstrap, &actions,
                                         NULL, arguments, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(registration[1]);
  close(browser_control[1]);
  close(ALLOY_GUEST_RESOURCE_FD);
  if (launched)
    return 125;
  int browser_pidfd = (int)syscall(SYS_pidfd_open, browser, 0);
  if (browser_pidfd < 0) {
    // This is the unreaped direct child from our own spawn, so it cannot have
    // been recycled. No numeric PID from the guest is trusted here.
    kill(browser, SIGKILL);
    waitpid(browser, NULL, 0);
    return 125;
  }
  struct members members = {NULL, 0};
  struct forwarding to_browser = {{0}, 0, 0};
  struct forwarding to_app = {{0}, 0, 0};
  int closing = 0;
  int browser_dead = 0;
  int browser_eof = 0;
  int result = 0;
  for (;;) {
    struct pollfd events[] = {
        {ALLOY_GUEST_CONTROL_FD, (short)((to_browser.count ? 0 : POLLIN) |
                                         (to_app.count ? POLLOUT : 0)), 0},
        {(browser_dead && to_app.count) || browser_eof ? -1 : browser_control[0],
                                (short)((to_app.count ? 0 : POLLIN) |
                                      (to_browser.count ? POLLOUT : 0)), 0},
        {registration[0], POLLIN, 0},
        {browser_dead ? -1 : browser_pidfd, POLLIN, 0}};
    int observed = poll(events, 4, -1);
    if (observed < 0 && errno == EINTR)
      continue;
    if (observed < 0)
      break;
    // App close/death is processed before registration in this same sequence.
    // A later entry cannot receive its admission ack once closing begins.
    if (events[0].revents & (POLLHUP | POLLERR | POLLNVAL))
      break;
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
    if (registration[0] >= 0 && events[2].revents) {
      if (!admit(registration[0], &members))
        break;
    }
    if (browser_dead && browser_eof && !to_app.count)
      break;
    for (size_t i = 0; i < members.count;) {
      struct pollfd event = {members.items[i].pidfd, POLLIN, 0};
      if (poll(&event, 1, 0) == 1 && event.revents) {
        close(members.items[i].pidfd);
        close(members.items[i].liveness);
        members.items[i] = members.items[--members.count];
      } else {
        ++i;
      }
    }
    reap();
  }
  // Closing the registration receiver first refuses even entries already
  // queued in the kernel. They are still in the pre-engine admission handshake.
  if (registration[0] >= 0)
    close(registration[0]);
  close(browser_control[0]);
  syscall(SYS_pidfd_send_signal, browser_pidfd, SIGKILL, NULL, 0);
  retire(&members);
  struct pollfd event = {browser_pidfd, POLLIN, 0};
  while (poll(&event, 1, -1) < 0 && errno == EINTR) {}
  close(browser_pidfd);
  reap();
  close(ALLOY_GUEST_CONTROL_FD);
  return result;
}
