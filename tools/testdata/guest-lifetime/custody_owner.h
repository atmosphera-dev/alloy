// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Faults apply only to the app-side fixture, never the static children.
#include <atomic>
#include <chrono>

static std::atomic<int> refused_waits{0}, refused_forces{0};
static std::atomic<int> wait_failures{0}, force_failures{0};
static std::atomic<int> refused_wait_handle{-1}, retried_wait_handle{-1};
static std::atomic<bool> fail_app_poll{false}, fail_app_recv{false};
static std::atomic<bool> fail_mask_restore{false};
static pid_t fixture_process = 0;
extern "C" int __real_sigprocmask(int, const sigset_t*, sigset_t*);
extern "C" int __wrap_sigprocmask(int how, const sigset_t* mask, sigset_t* old) {
  if (getpid() == fixture_process && how == SIG_SETMASK && mask &&
      !sigismember(mask, SIGINT) && fail_mask_restore.exchange(false)) {
    errno = EPERM;
    return -1;
  }
  return __real_sigprocmask(how, mask, old);
}
extern "C" int __real_poll(struct pollfd*, nfds_t, int);
extern "C" int __wrap_poll(struct pollfd* fds, nfds_t count, int timeout) {
  if (fail_app_poll.exchange(false)) { errno = EIO; return -1; }
  return __real_poll(fds, count, timeout);
}
extern "C" ssize_t __wrap_recvmsg(int fd, struct msghdr* message, int flags) {
  if (fail_app_recv.exchange(false)) { errno = EIO; return -1; }
  return recvmsg(fd, message, flags);
}
extern "C" int __real_waitid(idtype_t, id_t, siginfo_t*, int);
extern "C" int __wrap_waitid(idtype_t type, id_t id, siginfo_t* info, int flags) {
  if (type == static_cast<idtype_t>(3) && !(flags & WNOHANG)) {
    if (refused_waits.load() > 0 && refused_waits.fetch_sub(1) > 0) {
      refused_wait_handle = static_cast<int>(id);
      ++wait_failures;
      errno = EPERM;
      return -1;
    }
    if (wait_failures.load()) retried_wait_handle = static_cast<int>(id);
  }
  return __real_waitid(type, id, info, flags);
}
#ifdef ALLOY_LIFETIME_WAIT_OWNER
extern "C" int __real_alloy_pinned_signal(int, int);
extern "C" int __wrap_alloy_pinned_signal(int fd, int number) {
  if (number == SIGKILL && refused_forces.load() > 0 && refused_forces.fetch_sub(1) > 0) {
    ++force_failures;
    return EPERM;
  }
  return __real_alloy_pinned_signal(fd, number);
}

static void RequestFixtureForce(alloy::guest::Supervisor* child, bool cancelled = false) {
  // Model the public idempotent Terminate latch: a repeat does not submit a
  // second signal. Recovery must come from the original custody obligation.
  static bool termination_posted = false;
  if (termination_posted) return;
  termination_posted = true;
#ifdef ALLOY_LIFETIME_CUSTODY_OWNER
  child->RequestForce(cancelled);
#else
  child->forced = true;
  child->cancelled = cancelled;
  (void)__wrap_alloy_pinned_signal(child->pidfd, SIGKILL);
#endif
}

static void CustodyOwnerCase(alloy::guest::Supervisor* child, const std::string& check) {
  using namespace std::chrono_literals;
  std::promise<void> installed, resume, done;
  auto gate = resume.get_future();
  auto end = done.get_future();
  std::promise<alloy::guest::LaunchOutcome> outcome;
  auto launch = outcome.get_future();
  std::string unavailable;
  std::thread launcher([&] {
    auto result = alloy::guest::StartSupervisor(child, &unavailable, [&](alloy::guest::Supervisor*) {
      installed.set_value();
      if (check == "custody-caller-close") gate.wait();
      return alloy::guest::CustodyInstall::kInstalled;
    });
    outcome.set_value(result);
    end.wait();
  });
  assert(installed.get_future().wait_for(10s) == std::future_status::ready);
  if (check == "custody-caller-close") {
    assert(!__real_alloy_pinned_signal(child->pidfd, SIGSTOP));
    printf("CUSTODY_STOPPED %d\n", child->process);
    fflush(stdout);
    char confirmed;
    assert(read(STDIN_FILENO, &confirmed, 1) == 1 && confirmed == 'G');
    DenyLifetimeCall(SYS_pidfd_send_signal, EPERM, 1, SIGKILL);
    resume.set_value();
    RequestFixtureForce(child, true);
    assert(launch.wait_for(10s) == std::future_status::ready);
    assert(launch.get() != alloy::guest::LaunchOutcome::kReady);
  } else {
    assert(launch.wait_for(10s) == std::future_status::ready);
    assert(launch.get() == alloy::guest::LaunchOutcome::kReady);
    assert(alloy::guest::AdmitSupervisor(child->status_channel));
    if (check == "custody-caller-force") {
      assert(!__real_alloy_pinned_signal(child->runtime_pidfd, SIGSTOP));
      printf("CUSTODY_STOPPED %d\n", child->process);
      fflush(stdout);
      char confirmed;
      assert(read(STDIN_FILENO, &confirmed, 1) == 1 && confirmed == 'G');
      DenyLifetimeCall(SYS_pidfd_send_signal, EPERM, 1, SIGKILL);
    }
    if (check == "custody-late-wait") refused_waits = 2;
    if (check == "custody-force-retry") refused_forces = 1;
    RequestFixtureForce(child);
    if (check == "custody-force-retry") {
      auto deadline = std::chrono::steady_clock::now() + 10s;
      while (!force_failures.load()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
      }
      RequestFixtureForce(child);
    }
  }
  auto reap = std::async(std::launch::async, [&] { return alloy::guest::ReapSupervisor(child); });
  assert(reap.wait_for(10s) == std::future_status::ready);
  int result = reap.get();
  assert(result == (check == "custody-late-wait" || check == "custody-force-retry" ? -1 : 0));
  if (check == "custody-late-wait") {
    assert(wait_failures == 2);
    assert(refused_wait_handle >= 0 && retried_wait_handle == refused_wait_handle);
  }
  // All process threads, not just this caller, must have no remaining child.
  DIR* tasks = opendir("/proc/self/task");
  assert(tasks);
  while (auto* entry = readdir(tasks)) {
    if (entry->d_name[0] == '.') continue;
    std::string path = std::string("/proc/self/task/") + entry->d_name + "/children";
    FILE* children = fopen(path.c_str(), "r");
    if (!children) { assert(errno == ENOENT); continue; }
    int process;
    assert(fscanf(children, "%d", &process) == EOF);
    assert(!fclose(children));
  }
  assert(!closedir(tasks));
  done.set_value();
  launcher.join();
  puts("CUSTODY_OWNER_PASS");
}
#endif
