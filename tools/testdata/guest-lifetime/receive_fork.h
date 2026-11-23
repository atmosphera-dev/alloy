// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Interpose only this standalone app, not any static child or engine.
#include <atomic>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>

static std::atomic<bool> receive_fork_enabled{false};
static int receive_entered[2], receive_release[2], fork_entered[2], fork_result[2];
static int received_handle = -1;

static char ReadByte(int fd) {
  char byte;
  ssize_t count;
  do { count = read(fd, &byte, 1); } while (count < 0 && errno == EINTR);
  assert(count == 1);
  return byte;
}

extern "C" ssize_t recvmsg(int fd, struct msghdr* message, int flags) {
  auto receive = reinterpret_cast<ssize_t (*)(int, struct msghdr*, int)>(dlsym(RTLD_NEXT, "recvmsg"));
  assert(receive);
  ssize_t count = receive(fd, message, flags);
  auto* header = CMSG_FIRSTHDR(message);
  if (count > 0 && header && header->cmsg_level == SOL_SOCKET &&
      header->cmsg_type == SCM_RIGHTS && header->cmsg_len == CMSG_LEN(sizeof(int)) &&
      receive_fork_enabled.exchange(false)) {
    memcpy(&received_handle, CMSG_DATA(header), sizeof(received_handle));
    assert(write(receive_entered[1], "R", 1) == 1);
    assert(ReadByte(receive_release[0]) == 'G');
  }
  return count;
}

static void ForkEntered() { assert(write(fork_entered[1], "F", 1) == 1); }

static void ReceiveFork(alloy::guest::Supervisor* child) {
  for (int* pair : {receive_entered, receive_release, fork_entered, fork_result})
    assert(!pipe2(pair, O_CLOEXEC));
  // Registered after the product hook, so this prepare callback executes first.
  assert(!pthread_atfork(ForkEntered, nullptr, nullptr));
  receive_fork_enabled = true;
  bool launched = false;
  std::thread worker([&] { launched = alloy::guest::LaunchSupervisor(child); });
  assert(ReadByte(receive_entered[0]) == 'R');
  std::thread copier([&] {
    pid_t copy = fork();
    assert(copy >= 0);
    if (!copy) {
      char result = fcntl(received_handle, F_GETFD) == -1 && errno == EBADF ? 'F' : 'L';
      assert(write(fork_result[1], &result, 1) == 1);
      _exit(0);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(copy, &status, 0); } while (waited < 0 && errno == EINTR);
    assert(waited == copy || (waited < 0 && errno == ECHILD));
  });
  assert(ReadByte(fork_entered[0]) == 'F');
  // On old code the fork runs while receive is held; on fixed code it blocks
  // at the product prepare hook until publication. In either case the child's
  // actual descriptor check, not a scheduling timeout, decides success.
  pollfd result = {fork_result[0], POLLIN, 0};
  int observed;
  do { observed = poll(&result, 1, 1000); } while (observed < 0 && errno == EINTR);
  assert(observed >= 0);
  assert(write(receive_release[1], "G", 1) == 1);
  worker.join();
  copier.join();
  char proof = ReadByte(fork_result[0]);
  assert(launched);
#ifdef ALLOY_LIFETIME_WAIT_OWNER
  child->forced = true;
#endif
  assert(!syscall(SYS_pidfd_send_signal, child->pidfd, SIGKILL, nullptr, 0));
#ifdef ALLOY_LIFETIME_WAIT_OWNER
  assert(alloy::guest::ReapSupervisor(child) == 0);
#else
  alloy::guest::ReapSupervisor(child);
#endif
  for (int* pair : {receive_entered, receive_release, fork_entered, fork_result}) {
    close(pair[0]);
    close(pair[1]);
  }
  if (proof != 'F') fputs("FORK_DURING_RECEIVE_RETAINED_CUSTODY\n", stderr);
  assert(proof == 'F');
}
