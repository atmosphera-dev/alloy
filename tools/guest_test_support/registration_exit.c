// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// White-box boundary fixture: exercise credentials and ack races with real
// original process identities, independently of browser startup timing.
#define _GNU_SOURCE
#include "brave/alloy/guest/bootstrap/supervisor.c"
#include <assert.h>
#include <stdio.h>

static void registration_case(int exit_before_admit) {
  int registration[2], ready[2], release[2];
  assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, registration));
  assert(!pipe(ready) && !pipe(release));
  int enabled = 1;
  assert(!setsockopt(registration[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    close(registration[0]);
    close(ready[0]);
    close(release[1]);
    int endpoint[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, endpoint));
    uint32_t packet[] = {ALLOY_GUEST_REGISTRATION_MAGIC, 0};
    struct iovec io = {packet, sizeof(packet)};
    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct msghdr message = {0};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &endpoint[1], sizeof(int));
    assert(sendmsg(registration[1], &message, MSG_NOSIGNAL) == sizeof(packet));
    close(endpoint[0]);
    close(endpoint[1]);
    assert(write(ready[1], "R", 1) == 1);
    if (!exit_before_admit) {
      char byte;
      assert(read(release[0], &byte, 1) == 1);
    }
    _exit(0);
  }
  close(registration[1]);
  close(ready[1]);
  close(release[0]);
  char byte;
  assert(read(ready[0], &byte, 1) == 1);
  if (exit_before_admit) assert(waitpid(child, NULL, 0) == child);
  struct members members = {NULL, 0};
  assert(admit(registration[0], &members));
  if (exit_before_admit) {
    assert(members.count == 0);
  } else {
    // A disconnected but still-live original must remain supervised.
    assert(members.count == 1 && members.items[0].process == child);
    struct pollfd live = {members.items[0].pidfd, POLLIN, 0};
    assert(poll(&live, 1, 0) == 0);
    assert(write(release[1], "X", 1) == 1);
    assert(waitpid(child, NULL, 0) == child);
    retire(&members);
  }
  close(registration[0]);
  close(ready[0]);
  close(release[1]);
}

int main(void) {
  registration_case(1);
  registration_case(0);
  puts("GUEST_REGISTRATION_EXIT_PASS");
  return 0;
}
