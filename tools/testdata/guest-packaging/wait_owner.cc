// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/supervisor.h"
#include "base/process/alloy_guest_runtime.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

void ReapAppChildren(int) {
  int saved = errno;
  while (waitpid(-1, nullptr, WNOHANG) > 0) {}
  errno = saved;
}

int main(int argc, char** argv) {
  assert(argc == 2);
  std::string mode = argv[1];
  std::string policy = mode.starts_with("failed-") ? mode.substr(7) : mode;
  struct sigaction action = {};
  action.sa_handler = policy == "ignored" ? SIG_IGN : policy == "reaper" ? ReapAppChildren : SIG_DFL;
  action.sa_flags = policy == "nochildwait" ? SA_NOCLDWAIT : 0;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGCHLD, &action, nullptr));
  assert(!sigaction(SIGCHLD, nullptr, &action));
  sigset_t before_mask;
  assert(!sigprocmask(SIG_SETMASK, nullptr, &before_mask));
  alloy::guest::Supervisor child;
  std::string unavailable;
  if (mode == "close-receipt") {
    // Cancellation must leave no unread payload at the private receipt owner.
    // Its write side remains usable until it sends the final kernel status.
    int channel[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel));
    pid_t owner = fork();
    assert(owner >= 0);
    if (!owner) {
      close(channel[0]);
      char byte;
      if (recv(channel[1], &byte, 1, 0) != 0) _exit(125);
      const alloy_guest_wait_status packet = {
          ALLOY_GUEST_WAIT_STATUS_MAGIC, ALLOY_GUEST_WAIT_FINISHED, 0, 0,
          ALLOY_GUEST_WAIT_SETUP};
      _exit(send(channel[1], &packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet) ? 0 : 125);
    }
    close(channel[1]);
    child.process = owner;
    child.pidfd = static_cast<int>(syscall(SYS_pidfd_open, owner, 0));
    assert(child.pidfd >= 0);
    child.status_channel = channel[0];
    assert(alloy::guest::ReapSupervisor(&child) == 0);
    puts("CANCELLATION_EOF_PRESERVES_FINAL_RECEIPT=1");
  } else if (mode == "owner-dead") {
    // This disposable test app adopts/reaps the trusted fixture's orphan.
    assert(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    int barrier[2];
    assert(!pipe(barrier));
    assert(dup2(barrier[1], 203) == 203);
    close(barrier[1]);
    assert(alloy::guest::LaunchSupervisor(&child, &unavailable));
    char ready;
    assert(read(barrier[0], &ready, 1) == 1 && ready == 'R');
    int runtime = fcntl(child.runtime_pidfd, F_DUPFD_CLOEXEC, 3);
    assert(runtime >= 0);
    assert(!syscall(SYS_pidfd_send_signal, child.pidfd, SIGKILL, nullptr, 0));
    assert(alloy::guest::ReapSupervisor(&child) == -1);
    pollfd exited = {runtime, POLLIN, 0};
    assert(poll(&exited, 1, 0) == 1 && (exited.revents & POLLIN));
    close(runtime); close(barrier[0]); close(203);
    while (waitpid(-1, nullptr, 0) > 0) {}
    puts("OWNER_LOSS_RETAINS_RUNTIME_EXIT_BARRIER=1");
  } else if (mode == "denied") {
    assert(!alloy::guest::LaunchSupervisor(&child, &unavailable));
    assert(unavailable == "process policy denies pidfd_send_signal: identity-bound process teardown unavailable");
    assert(child.process == -1 && child.pidfd == -1 && child.status_channel == -1);
    puts("WAIT_OWNER_EXACT_SIGKILL_DENIAL_REFUSED=1");
  } else if (mode == "cancel-stopped") {
    int barrier[2];
    assert(!pipe(barrier));
    assert(dup2(barrier[1], 203) == 203);
    close(barrier[1]);
    int cancel = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(cancel >= 0);
    bool launched = true;
    std::thread worker([&] { launched = alloy::guest::LaunchSupervisor(&child, &unavailable, cancel); });
    char ready;
    assert(read(barrier[0], &ready, 1) == 1 && ready == 'S');
    uint64_t one = 1;
    assert(write(cancel, &one, sizeof(one)) == sizeof(one));
    worker.join();
    close(barrier[0]); close(203); close(cancel);
    assert(!launched && child.process == -1 && child.pidfd == -1 && child.status_channel == -1);
    puts("STOPPED_PROBE_CANCELLED_AND_REAPED=1");
  } else {
    assert(alloy::guest::LaunchSupervisor(&child, &unavailable));
    assert(unavailable.empty());
    int status = alloy::guest::ReapSupervisor(&child);
    assert(mode.starts_with("failed") ? WIFEXITED(status) && WEXITSTATUS(status) == 125 : status == 0);
    printf("APP_SIGCHLD_MODE=%s VERIFIED_STATUS=%d\n", mode.c_str(), status);
  }
  struct sigaction after;
  sigset_t after_mask;
  assert(!sigaction(SIGCHLD, nullptr, &after));
  assert(!sigprocmask(SIG_SETMASK, nullptr, &after_mask));
  assert(after.sa_handler == action.sa_handler);
  assert(after.sa_flags == action.sa_flags);
  for (int signal = 1; signal < NSIG; ++signal) {
    assert(sigismember(&before_mask, signal) == sigismember(&after_mask, signal));
    assert(sigismember(&action.sa_mask, signal) == sigismember(&after.sa_mask, signal));
  }
  puts("APP_SIGNAL_SETUP_UNCHANGED=1");
}
