// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Standalone consumer of the production app-side launch/reap implementation.
#include "brave/alloy/guest/session/supervisor.h"
#include "denial.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "receive_fork.h"
#include "custody_owner.h"

static void Reaper(int) {
  int saved = errno;
  while (waitpid(-1, nullptr, WNOHANG) > 0) {}
  errno = saved;
}

static const char* const namespaces[] = {"/proc/self/ns/user", "/proc/self/ns/pid", "/proc/self/ns/mnt"};
static struct stat namespace_before[3];
static pid_t session_before;

static void VerifySignals(const struct sigaction& before) {
  struct sigaction after;
  sigset_t mask;
  assert(!sigaction(SIGCHLD, nullptr, &after));
  assert(after.sa_handler == before.sa_handler && after.sa_flags == before.sa_flags);
  assert(!sigprocmask(SIG_SETMASK, nullptr, &mask));
  assert(sigismember(&mask, SIGTERM) == 1);
  assert(!sigaction(SIGHUP, nullptr, &after) && after.sa_handler == SIG_IGN);
  assert(getsid(0) == session_before);
  for (size_t i = 0; i < 3; ++i) {
    struct stat current;
    assert(!stat(namespaces[i], &current));
    assert(current.st_dev == namespace_before[i].st_dev && current.st_ino == namespace_before[i].st_ino);
  }
}

static size_t DescriptorCount() {
  DIR* directory = opendir("/proc/self/fd");
  assert(directory);
  size_t count = 0;
  while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
  assert(!closedir(directory));
  return count;
}

static void VerifyNoChildren() {
  FILE* children = fopen("/proc/thread-self/children", "r");
  assert(children);
  int process;
  assert(fscanf(children, "%d", &process) == EOF);
  assert(!fclose(children));
}

int main(int argc, char** argv) {
  assert(argc == 2 || argc == 3);
  std::string check = argc == 3 ? argv[2] : "";
  struct sigaction action = {};
  action.sa_handler = !strcmp(argv[1], "ignored") ? SIG_IGN :
                      !strcmp(argv[1], "reaper") ? Reaper : SIG_DFL;
  action.sa_flags = !strcmp(argv[1], "nochildwait") ? SA_NOCLDWAIT : 0;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGCHLD, &action, nullptr));
  struct sigaction before;
  assert(!sigaction(SIGCHLD, nullptr, &before));
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  assert(!sigprocmask(SIG_BLOCK, &blocked, nullptr));
  assert(signal(SIGHUP, SIG_IGN) != SIG_ERR);
  session_before = getsid(0);
  for (size_t i = 0; i < 3; ++i) assert(!stat(namespaces[i], &namespace_before[i]));
  if (check == "clone-enosys") DenyLifetimeCall(SYS_clone3, ENOSYS);
  if (check == "waitid-denied") DenyLifetimeCall(SYS_waitid, EPERM, 0, 3);
  if (check == "kill-denied" || check == "kill-refused-probe")
    DenyLifetimeCall(SYS_pidfd_send_signal, EPERM, 1, SIGKILL);
  if (check == "socket-denied") DenyLifetimeCall(SYS_socketpair, EACCES);
  if (check == "mask-restore-denied") {
    fixture_process = getpid();
    fail_mask_restore = true;
  }
  if (check == "fd-pressure") {
    rlimit original;
    assert(!getrlimit(RLIMIT_NOFILE, &original) && original.rlim_max >= 256);
    rlimit limited = original;
    limited.rlim_cur = 256;  // Still permits all fixed inheritance destinations.
    assert(!setrlimit(RLIMIT_NOFILE, &limited));
    std::vector<int> reserved;
    for (;;) {
      int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (fd < 0) { assert(errno == EMFILE); break; }
      reserved.push_back(fd);
    }
    for (int i = 0; i < 6; ++i) { close(reserved.back()); reserved.pop_back(); }
    size_t descriptors = DescriptorCount();
    for (int attempt = 0; attempt < 16; ++attempt) {
      {
      alloy::guest::Supervisor pressure;
#ifdef ALLOY_LIFETIME_WAIT_OWNER
      std::string unavailable;
      auto outcome = alloy::guest::StartSupervisor(&pressure, &unavailable, [](alloy::guest::Supervisor*) {
        return alloy::guest::CustodyInstall::kInstalled;
      });
      assert(outcome != alloy::guest::LaunchOutcome::kUnavailable && unavailable.empty());
      if (outcome == alloy::guest::LaunchOutcome::kReady)
        assert(alloy::guest::AdmitSupervisor(pressure.status_channel));
      assert(alloy::guest::ReapSupervisor(&pressure) == 0);
#else
      alloy::guest::LaunchSupervisor(&pressure);
      alloy::guest::ReapSupervisor(&pressure);
#endif
      }
      assert(DescriptorCount() == descriptors);
      VerifyNoChildren();
    }
    for (int fd : reserved) close(fd);
    assert(!setrlimit(RLIMIT_NOFILE, &original));
    VerifySignals(before);
    puts("FD_PRESSURE_ACCOUNTED APP_SIGNALS_UNCHANGED");
    return 0;
  }
  int registry_result = -1;
  bool registry = check.starts_with("registration-");
  bool observed_fault = check.starts_with("fault-inner-") || check == "eintr-registration";
  if (registry || observed_fault) {
    int barrier[2];
    assert(!pipe2(barrier, O_CLOEXEC));
    assert(dup2(barrier[1], 203) == 203);
    close(barrier[1]);
    registry_result = barrier[0];
  }
  alloy::guest::Supervisor child;
  if (check.starts_with("custody-")) {
#ifdef ALLOY_LIFETIME_WAIT_OWNER
    CustodyOwnerCase(&child, check);
    VerifySignals(before);
    return 0;
#else
    puts("CUSTODY_OWNER_UNAVAILABLE");
    return 4;
#endif
  }
  if (check == "receive-fork") {
    ReceiveFork(&child);
    VerifySignals(before);
    puts("RECEIVE_FORK_CLOSED APP_SIGNALS_UNCHANGED");
    return 0;
  }
  if (check == "startup-cancel") {
#ifdef ALLOY_LIFETIME_WAIT_OWNER
    std::promise<int> installed;
    auto original = installed.get_future();
    std::atomic<bool> delivered{false};
    alloy::guest::LaunchOutcome outcome = alloy::guest::LaunchOutcome::kFailed;
    std::thread worker([&] {
      std::string unavailable;
      outcome = alloy::guest::StartSupervisor(&child, &unavailable, [&](alloy::guest::Supervisor* value) {
        delivered = true;
        installed.set_value(value->pidfd);
        printf("CREATED %d\n", child.process);
        fflush(stdout);
        return alloy::guest::CustodyInstall::kInstalled;
      });
      if (!delivered.exchange(true)) installed.set_value(-1);
    });
    int handle = original.get();
    assert(handle >= 0);
    char command = 0;
    while (read(STDIN_FILENO, &command, 1) < 0 && errno == EINTR) {}
    assert(command == 'K');
    child.forced = true;
    child.cancelled = true;
    assert(!syscall(SYS_pidfd_send_signal, handle, SIGKILL, nullptr, 0));
    worker.join();
    assert(outcome != alloy::guest::LaunchOutcome::kReady);
    assert(alloy::guest::ReapSupervisor(&child) == 0);
#else
    assert(alloy::guest::LaunchSupervisor(&child));
    printf("CREATED %d\n", child.process);
    fflush(stdout);
    char command = 0;
    while (read(STDIN_FILENO, &command, 1) < 0 && errno == EINTR) {}
    assert(command == 'K');
    assert(!syscall(SYS_pidfd_send_signal, child.pidfd, SIGKILL, nullptr, 0));
    alloy::guest::ReapSupervisor(&child);
#endif
    VerifySignals(before);
    puts("CANCELLED_CLOSED APP_SIGNALS_UNCHANGED");
    return 0;
  }
#ifdef ALLOY_LIFETIME_WAIT_OWNER
  std::string unavailable;
  auto outcome = alloy::guest::StartSupervisor(&child, &unavailable, [](alloy::guest::Supervisor*) {
    return alloy::guest::CustodyInstall::kInstalled;
  });
  bool launched = outcome == alloy::guest::LaunchOutcome::kReady &&
                   (check == "kill-refused-retire" || alloy::guest::AdmitSupervisor(child.status_channel));
  if (outcome == alloy::guest::LaunchOutcome::kUnavailable) {
    printf("REFUSED %s\n", unavailable.c_str());
    fflush(stdout);
    return alloy::guest::ReapSupervisor(&child) == 0 ? 0 : 3;
  }
#else
  bool launched = alloy::guest::LaunchSupervisor(&child);
#endif
  if (launched && check == "fault-app-poll") fail_app_poll = true;
  if (launched && check == "fault-app-recv") fail_app_recv = true;
  if (launched && check == "fault-app-wait") refused_waits = 1;
  printf("LAUNCHED %d OWNER %d\n", launched, child.process);
  fflush(stdout);
  if (!launched) return 2;
  if (registry) {
    close(203);
    char result = 0;
    ssize_t count;
    do { count = read(registry_result, &result, 1); } while (count < 0 && errno == EINTR);
    close(registry_result);
#ifdef ALLOY_LIFETIME_WAIT_OWNER
    int cleanup = alloy::guest::ReapSupervisor(&child);
    if (count == 1 && result == 'R') assert(cleanup == 0);
#else
    alloy::guest::ReapSupervisor(&child);
#endif
    VerifySignals(before);
    puts(count == 1 && result == 'R' ? "REGISTRY_PASS APP_SIGNALS_UNCHANGED" : "REGISTRY_FAILED");
    return count == 1 && result == 'R' ? 0 : 4;
  }
  if (observed_fault) {
    close(203);
    char result = 0;
    ssize_t count;
    do { count = read(registry_result, &result, 1); } while (count < 0 && errno == EINTR);
    close(registry_result);
    assert(count == 1 && result == 'E');
    puts("FAULT_OBSERVED");
    fflush(stdout);
  }
  char command = 0;
  while (read(STDIN_FILENO, &command, 1) < 0 && errno == EINTR) {}
  if (check.starts_with("fault-") || check == "kill-refused-retire") {
    assert(command == 'C');
#ifdef ALLOY_LIFETIME_WAIT_OWNER
    int cleanup = alloy::guest::ReapSupervisor(&child);
    assert(alloy::guest::ReapSupervisor(&child) == cleanup);
#else
    alloy::guest::ReapSupervisor(&child);
    int cleanup = 0;
#endif
    VerifySignals(before);
    VerifyNoChildren();
    puts(cleanup == -1 ? "FAILED_CLEANUP APP_SIGNALS_UNCHANGED" : "FALSE_CLEANUP_SUCCESS");
    return cleanup == -1 ? 0 : 4;
  }
  pid_t copy = -1;
  if (command == 'K') {
    assert(!syscall(SYS_pidfd_send_signal, child.pidfd, SIGKILL, nullptr, 0));
  } else if (command == 'F') {
    int inherited[] = {child.pidfd, child.channel, child.resources,
#ifdef ALLOY_LIFETIME_WAIT_OWNER
                       child.status_channel, child.exec_error, child.runtime_pidfd,
                       child.probe_pidfd, child.mirror_status,
#ifdef ALLOY_LIFETIME_CUSTODY_OWNER
                       child.request_channel,
#endif
#endif
    };
    int barrier[2];
    assert(!pipe2(barrier, O_CLOEXEC));
    copy = fork();
    assert(copy >= 0);
    if (!copy) {
      close(barrier[0]);
      for (int handle : inherited) {
        if (handle >= 0 && (fcntl(handle, F_GETFD) >= 0 || errno != EBADF)) {
          fputs("FORK_COPY_RETAINED_CUSTODY\n", stderr);
          _exit(5);
        }
      }
      assert(write(barrier[1], "F", 1) == 1);
      close(barrier[1]);
      sleep(60);
      _exit(0);
    }
    close(barrier[1]);
    char confirmed = 0;
    ssize_t count;
    do { count = read(barrier[0], &confirmed, 1); } while (count < 0 && errno == EINTR);
    close(barrier[0]);
    assert(count == 1 && confirmed == 'F');
    printf("COPY %d\n", copy);
    fflush(stdout);
  }
#ifdef ALLOY_LIFETIME_WAIT_OWNER
  int cleanup = alloy::guest::ReapSupervisor(&child);
  assert(cleanup == (command == 'K' || check == "inner-lost" ? -1 : 0));
#else
  alloy::guest::ReapSupervisor(&child);
#endif
  VerifySignals(before);
  if (copy > 0) assert(waitpid(copy, nullptr, WNOHANG) == 0);
  puts("REAPED APP_SIGNALS_UNCHANGED");
}
