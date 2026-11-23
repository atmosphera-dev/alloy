// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Runs in a fixture-only user/PID/mount namespace, never in the product entry.
#define _GNU_SOURCE
#include "brave/alloy/guest/bootstrap/supervisor.c"
#include <assert.h>
#include <stdio.h>

int alloy_lock_prepare_scratch(void) { return 1; }
int alloy_lock_scratch_valid(void) { return 1; }
void alloy_lock_remove_scratch(void) {}

static int fail_registry;
void* __real_realloc(void* pointer, size_t size);
void* __wrap_realloc(void* pointer, size_t size) {
  if (fail_registry) { errno = ENOMEM; return NULL; }
  return __real_realloc(pointer, size);
}

static void send_peer(int lane, int stopped) {
  int pair[2];
  assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
  uint32_t packet[] = {ALLOY_GUEST_REGISTRATION_MAGIC, 0};
  struct iovec io = {packet, sizeof(packet)};
  union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control;
  memset(&control, 0, sizeof(control));
  struct msghdr message = {0};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &pair[1], sizeof(int));
  assert(sendmsg(lane, &message, MSG_NOSIGNAL) == sizeof(packet));
  close(pair[1]);
  if (stopped) {
    raise(SIGSTOP);
    for (;;) pause();
  }
  close(pair[0]);
  _exit(0);
}

static int readable(int fd, int milliseconds) {
  struct pollfd event = {fd, POLLIN, 0};
  int result;
  do { result = poll(&event, 1, milliseconds); } while (result < 0 && errno == EINTR);
  assert(result >= 0);
  return (event.revents & POLLIN) != 0;
}

static void clean_child(int fd, pid_t process) {
  int result = (int)syscall(SYS_pidfd_send_signal, fd, SIGKILL, NULL, 0);
  assert(!result || errno == ESRCH);
  int status;
  pid_t waited;
  do { waited = waitpid(process, &status, 0); } while (waited < 0 && errno == EINTR);
  assert(waited == process || (waited == -1 && errno == ECHILD));
  close(fd);
}

int main(int argc, char** argv) {
  assert(argc == 2 && getpid() == 1);
  struct sigaction action = {0};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGCHLD, &action, NULL));
  int lane[2], enabled = 1;
  assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, lane));
  assert(!setsockopt(lane[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
  int allocation = !strcmp(argv[1], "allocation");
  int duplicate = !strcmp(argv[1], "duplicate");
  pid_t registrant = fork();
  assert(registrant >= 0);
  if (!registrant) send_peer(lane[1], allocation);
  struct members members = {NULL, 0};
  int good;
  if (allocation) {
    int status;
    assert(waitpid(registrant, &status, WUNTRACED) == registrant && WIFSTOPPED(status));
    int original = (int)syscall(SYS_pidfd_open, registrant, 0);
    assert(original >= 0);
    fail_registry = 1;
    int accepted = admit(lane[0], &members);
    fail_registry = 0;
    good = !accepted && members.count == 0 && readable(original, 3000);
    clean_child(original, registrant);
  } else {
    int old = (int)syscall(SYS_pidfd_open, registrant, 0);
    assert(old >= 0);
    int status;
    assert(waitpid(registrant, &status, 0) == registrant);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (duplicate) {
      // Drain the queued, exited registration, then simulate a retained entry
      // whose original status was consumed by its native creator.
      assert(admit(lane[0], &members));
      members.items = malloc(sizeof(struct member));
      assert(members.items);
      members.items[0] = (struct member){old, -1, registrant};
      members.count = 1;
    } else close(old);
    // Deterministically reuse the just-reaped numeric PID. Queued credentials
    // and SO_PEERCRED still name the exited original; only the live-peer proof
    // can prevent the replacement from entering the retirement registry.
    FILE* next = fopen("/proc/sys/kernel/ns_last_pid", "w");
    assert(next);
    assert(fprintf(next, "%d\n", registrant - 1) > 0);
    assert(!fclose(next));
    pid_t sentinel = fork();
    assert(sentinel >= 0);
    if (!sentinel) {
      if (duplicate) send_peer(lane[1], 1);
      for (;;) pause();
    }
    assert(sentinel == registrant);
    int original = (int)syscall(SYS_pidfd_open, sentinel, 0);
    assert(original >= 0);
    if (duplicate) {
      assert(waitpid(sentinel, &status, WUNTRACED) == sentinel && WIFSTOPPED(status));
    }
    int accepted = admit(lane[0], &members);
    if (duplicate) {
      good = accepted && members.count == 1 && !readable(members.items[0].pidfd, 0);
      // Preserve the failing observation before fixture cleanup. Old retire()
      // would otherwise block on the stopped replacement it failed to admit.
      if (!good) {
        clean_child(original, sentinel);
        original = -1;
      }
      retire(&members);
      if (original >= 0) {
        good = good && readable(original, 0);
        clean_child(original, sentinel);
      }
    } else {
      int unregistered = members.count == 0;
      retire(&members);
      good = accepted && unregistered && !readable(original, 0) &&
             waitpid(sentinel, NULL, WNOHANG) == 0;
      clean_child(original, sentinel);
    }
  }
  close(lane[0]);
  close(lane[1]);
  assert(write(203, good ? "R" : "F", 1) == 1);
  return good ? 0 : 1;
}
