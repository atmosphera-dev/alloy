// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/supervisor.h"
#include "brave/alloy/guest/linux_lock/pinned_spawn.h"
#include "base/process/alloy_guest_runtime.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace alloy::guest {
namespace {
pthread_mutex_t fork_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t fork_once = PTHREAD_ONCE_INIT;
Supervisor* fork_head = nullptr;
int fork_error = 0;
void BeforeAppFork() { pthread_mutex_lock(&fork_mutex); }
void AfterAppFork() { pthread_mutex_unlock(&fork_mutex); }
void InAppFork() {
  // Only close and fixed memory stores in the child. No socket shutdown: that
  // would also shut down the parent's session. Raw clone3 does not call atfork.
  for (Supervisor* owner = fork_head; owner; owner = owner->fork_next) {
    for (int* fd : {&owner->pidfd, &owner->channel, &owner->resources,
                   &owner->status_channel, &owner->exec_error, &owner->runtime_pidfd,
                   &owner->mirror_pidfd, &owner->mirror_status, &owner->probe_pidfd}) {
      if (*fd >= 0) close(*fd);
      *fd = -1;
    }
  }
  fork_head = nullptr;
  pthread_mutex_unlock(&fork_mutex);
}
void InstallForkGuard() { fork_error = pthread_atfork(BeforeAppFork, AfterAppFork, InAppFork); }
}  // namespace

SupervisorForkLock::SupervisorForkLock() { pthread_mutex_lock(&fork_mutex); }
SupervisorForkLock::~SupervisorForkLock() { pthread_mutex_unlock(&fork_mutex); }
Supervisor::Supervisor() {
  pthread_once(&fork_once, InstallForkGuard);
  SupervisorForkLock guard;
  fork_next = fork_head;
  fork_head = this;
}
std::string BundledBootstrap() {
  Dl_info info = {};
  char path[PATH_MAX];
  if (!dladdr(reinterpret_cast<void*>(&BundledBootstrap), &info) ||
      !info.dli_fname || !realpath(info.dli_fname, path)) return {};
  std::string library(path);
  auto separator = library.find_last_of('/');
  if (separator == std::string::npos) return {};
  return library.substr(0, separator + 1) + "alloy_guest_host";
}

namespace {
constexpr int kProbeStatus = 64;
int CloseSocket(int* fd) {
  int error = 0;
  if (*fd >= 0) {
    int result;
    do { result = shutdown(*fd, SHUT_RDWR); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != ENOTCONN) error = errno;
    if (close(*fd) && !error) error = errno;
    *fd = -1;
  }
  return error;
}

// 1 packet, 0 drained, -1 invalid/failed, -2 EOF, -3 connection reset.
// Never block on bytes while an init
// pidfd may have become readable. Surplus SCM_RIGHTS are always closed.
int ReceiveStatus(Supervisor* child) {
  // SCM_RIGHTS creates descriptors before recvmsg returns. Keep app forks out
  // through validation/publication (or closure), not merely the final store.
  SupervisorForkLock fork_guard;
  alloy_guest_wait_status packet = {};
  iovec io = {&packet, sizeof(packet)};
  union { cmsghdr alignment; char bytes[CMSG_SPACE(253 * sizeof(int))]; } control = {};
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  ssize_t count;
  do { count = recvmsg(child->status_channel, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); }
  while (count < 0 && errno == EINTR);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  if (count < 0) return errno == ECONNRESET || errno == ENOTCONN ? -3 : -1;
  bool valid = count == sizeof(packet) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
               packet.magic == ALLOY_GUEST_WAIT_STATUS_MAGIC && packet.error >= 0 &&
               packet.operation <= ALLOY_GUEST_WAIT_REAP;
  int descriptor = -1;
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
      size_t size = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      auto* handles = reinterpret_cast<int*>(CMSG_DATA(header));
      for (size_t i = 0; i < size; ++i) {
        if (descriptor < 0) descriptor = handles[i];
        else { close(handles[i]); valid = false; }
      }
    } else valid = false;
  }
  if (count == 0) {
    if (descriptor >= 0) close(descriptor);
    return CMSG_FIRSTHDR(&message) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ? -1 : -2;
  }
  bool probe = packet.phase == ALLOY_GUEST_WAIT_PROBE;
  bool transfer = (packet.phase == ALLOY_GUEST_WAIT_READY || probe) && !packet.error;
  valid = valid && (packet.phase == ALLOY_GUEST_WAIT_READY || packet.phase == ALLOY_GUEST_WAIT_FINISHED || probe) &&
          (transfer ? descriptor >= 0 && (probe ? child->probe_pidfd : child->runtime_pidfd) < 0 : descriptor < 0) &&
          (!probe || (!packet.error && packet.operation == ALLOY_GUEST_WAIT_SIGNAL && packet.status == 0));
  if (!valid) {
    if (descriptor >= 0) close(descriptor);
    return -1;
  }
  if (transfer) {
    (probe ? child->probe_pidfd : child->runtime_pidfd) = descriptor;
  }
  if (probe) return kProbeStatus;
  if (packet.phase == ALLOY_GUEST_WAIT_FINISHED) {
    if (child->finished) return -1;
    child->finished = true;
    child->receipt_status = packet.error ? -1 : packet.status;
    if (packet.error) child->cleanup = -1;
  } else if (packet.error) {
    child->receipt_status = packet.error;
    // Preserve the precise failed capability separately from cleanup.
    return static_cast<int>(packet.operation) + 2;
  } else if (packet.status < 0 || !WIFSIGNALED(packet.status) || WTERMSIG(packet.status) != SIGKILL) {
    return -1;
  }
  return 1;
}

LaunchOutcome Unavailable(std::string* reason, const char* operation, int error) {
  // Resource exhaustion and corrupt/failed setup are launch failures, not an
  // assertion that the platform lacks a protection capability.
  if (error != EPERM && error != EACCES && error != ENOSYS && error != EINVAL &&
      error != E2BIG && error != ENOSPC) return LaunchOutcome::kFailed;
  *reason = std::string("guest lifetime unavailable: ") + operation + ": " + strerror(error);
  return LaunchOutcome::kUnavailable;
}

int SpawnBootstrap(const std::string& bootstrap, Supervisor* child) {
  SupervisorForkLock guard;
  int pair[2] = {-1, -1}, resources[2] = {-1, -1}, status[2] = {-1, -1};
  int error = 0;
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, resources) ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, status)) error = errno;
  if (!error) {
    std::vector<std::string> environment = {
        "PATH=/usr/bin:/bin", "LANG=C.UTF-8", "ALLOY_GUEST_RUNTIME=1",
        "ALLOY_GUEST_BOOTSTRAP=" + bootstrap, "GSETTINGS_BACKEND=memory", "GIO_USE_VFS=local",
        "DBUS_SESSION_BUS_ADDRESS=disabled:", "NO_AT_BRIDGE=1",
        "MESA_SHADER_CACHE_DISABLE=true", "__GL_SHADER_DISK_CACHE=0"};
    for (const char* name : {"HOME", "TMPDIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                            "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"}) {
      if (const char* value = getenv(name)) environment.emplace_back(std::string(name) + "=" + value);
    }
    std::vector<char*> env;
    for (auto& field : environment) env.push_back(field.data());
    env.push_back(nullptr);
    char* argv[] = {const_cast<char*>(bootstrap.c_str()),
                    const_cast<char*>(ALLOY_GUEST_WAIT_OWNER_ARGUMENT), nullptr};
    const alloy_lock_fd_map maps[] = {{pair[1], ALLOY_GUEST_CONTROL_FD},
        {resources[1], ALLOY_GUEST_RESOURCE_FD}, {status[1], ALLOY_GUEST_WAIT_STATUS_FD}};
    alloy_pinned_child pinned;
    error = alloy_spawn_pinned(bootstrap.c_str(), argv, env.data(), maps, 3, 1, &pinned);
    if (pinned.pidfd >= 0) {
      child->process = pinned.process;
      child->pidfd = pinned.pidfd;
      child->exec_error = pinned.exec_error;
      child->channel = pair[0]; pair[0] = -1;
      child->resources = resources[0]; resources[0] = -1;
      child->status_channel = status[0]; status[0] = -1;
    }
  }
  for (int fd : {pair[0], pair[1], resources[0], resources[1], status[0], status[1]})
    if (fd >= 0) close(fd);
  return error;
}
}  // namespace

Supervisor::~Supervisor() {
  if (pidfd >= 0) {
    forced = true;
    if (alloy_pinned_signal(pidfd, SIGKILL)) cleanup = -1;
    ReapSupervisor(this);
  }
  SupervisorForkLock guard;
  for (Supervisor** item = &fork_head; *item; item = &(*item)->fork_next) {
    if (*item == this) { *item = fork_next; break; }
  }
  for (int fd : {mirror_pidfd, mirror_status}) if (fd >= 0) close(fd);
}

LaunchOutcome StartSupervisor(Supervisor* child, std::string* unavailable,
                              const std::function<CustodyInstall(Supervisor*)>& install) {
  unavailable->clear();
  if (fork_error) return LaunchOutcome::kFailed;
  // Probe a real positive pidfd before creating a child. ECHILD for our own
  // identity proves parsing/policy without leaving a zombie on refused waitid.
  siginfo_t info = {};
  int checked;
  int wait_error;
  {
    SupervisorForkLock guard;
    int self = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (self < 0) return Unavailable(unavailable, "pidfd_open(parent liveness)", errno);
    do { checked = waitid(static_cast<idtype_t>(3), self, &info, WEXITED | WNOHANG | WNOWAIT); }
    while (checked < 0 && errno == EINTR);
    wait_error = errno;
    close(self);
  }
  if (checked != -1) return LaunchOutcome::kFailed;
  if (wait_error != ECHILD) return Unavailable(unavailable, "waitid(P_PIDFD)", wait_error);
  std::string bootstrap = BundledBootstrap();
  if (bootstrap.empty() || access(bootstrap.c_str(), X_OK)) return LaunchOutcome::kFailed;
  int error = SpawnBootstrap(bootstrap, child);
  if (child->pidfd >= 0) {
    CustodyInstall installed = install(child);
    if (installed != CustodyInstall::kInstalled) {
      child->forced = true;
      child->cancelled = installed == CustodyInstall::kCancelled;
      if (alloy_pinned_signal(child->pidfd, SIGKILL)) child->cleanup = -1;
      return child->cancelled ? LaunchOutcome::kCancelled : LaunchOutcome::kFailed;
    }
  }
  if (error) return Unavailable(unavailable, "clone3(CLONE_PIDFD|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNS)", error);
  // Wait only after publishing custody. A stopped pre-exec child is therefore
  // still cancellable. Read the error lane before interpreting owner exit, so
  // a fast map/exec refusal cannot lose its precise operation/errno report.
  for (;;) {
    pollfd events[] = {{child->exec_error, POLLIN, 0}, {child->pidfd, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 2, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0) { child->cleanup = -1; return LaunchOutcome::kFailed; }
    if (events[0].revents || events[1].revents) {
      const char* operation = "bootstrap exec receipt";
      error = alloy_pinned_exec_result(child->exec_error, &operation);
      {
        SupervisorForkLock guard;
        close(child->exec_error); child->exec_error = -1;
      }
      if (child->cancelled) return LaunchOutcome::kCancelled;
      if (error) return Unavailable(unavailable, operation, error);
      break;
    }
  }
  info = {};
  do { checked = waitid(static_cast<idtype_t>(3), child->pidfd, &info, WEXITED | WNOHANG | WNOWAIT); }
  while (checked < 0 && errno == EINTR);
  if (checked < 0) {
    if (child->cancelled) return LaunchOutcome::kCancelled;
    return errno == ECHILD ? LaunchOutcome::kOwnerLost : Unavailable(unavailable, "waitid(P_PIDFD)", errno);
  }
  if (info.si_pid) return child->cancelled ? LaunchOutcome::kCancelled : LaunchOutcome::kOwnerLost;
  ssize_t sent;
  do { sent = send(child->status_channel, "G", 1, MSG_NOSIGNAL | MSG_DONTWAIT); }
  while (sent < 0 && errno == EINTR);
  if (sent != 1) return child->cancelled ? LaunchOutcome::kCancelled : LaunchOutcome::kOwnerLost;
  for (;;) {
    pollfd events[] = {{child->status_channel, POLLIN, 0}, {child->pidfd, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 2, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0) { child->cleanup = -1; return LaunchOutcome::kFailed; }
    if (events[0].revents & POLLIN) {
      int received = ReceiveStatus(child);
      if (received == kProbeStatus) {
        int signaled = alloy_pinned_signal(child->probe_pidfd, SIGKILL);
        ssize_t acknowledged;
        do { acknowledged = send(child->status_channel, &signaled, sizeof(signaled), MSG_NOSIGNAL | MSG_DONTWAIT); }
        while (acknowledged < 0 && errno == EINTR);
        if (acknowledged != sizeof(signaled))
          return child->cancelled ? LaunchOutcome::kCancelled : LaunchOutcome::kFailed;
        continue;
      }
      if (received == 1 && child->runtime_pidfd >= 0) {
        if (fcntl(child->channel, F_SETFL, O_NONBLOCK) < 0) return LaunchOutcome::kFailed;
        return LaunchOutcome::kReady;
      }
      if (received >= 2) {
        const char* operation = received - 2 == ALLOY_GUEST_WAIT_SIGNAL ? "pidfd_send_signal(SIGKILL)" :
            received - 2 == ALLOY_GUEST_WAIT_REAP ? "waitid(P_PIDFD)" : "static custody setup";
        if (child->receipt_status == ECANCELED) return LaunchOutcome::kCancelled;
        return Unavailable(unavailable, operation, child->receipt_status);
      }
      return LaunchOutcome::kFailed;
    }
    if (events[0].revents || events[1].revents)
      return child->cancelled ? LaunchOutcome::kCancelled : LaunchOutcome::kOwnerLost;
  }
}

bool AdmitSupervisor(int status_channel) {
  ssize_t sent;
  do { sent = send(status_channel, "A", 1, MSG_NOSIGNAL | MSG_DONTWAIT); }
  while (sent < 0 && errno == EINTR);
  return sent == 1;
}

bool LaunchSupervisor(Supervisor* child) {
  std::string unavailable;
  return StartSupervisor(child, &unavailable, [](Supervisor*) { return CustodyInstall::kInstalled; }) == LaunchOutcome::kReady &&
         AdmitSupervisor(child->status_channel);
}

int ReapSupervisor(Supervisor* child) {
  if (child->pidfd < 0) return child->cleanup;
  {
    SupervisorForkLock guard;
    if (CloseSocket(&child->resources)) child->cleanup = -1;
    if (CloseSocket(&child->channel)) child->cleanup = -1;
  }
  if (child->status_channel >= 0) {
    int result;
    do { result = shutdown(child->status_channel, SHUT_WR); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != ENOTCONN) child->cleanup = -1;
  }
  bool drained = child->status_channel < 0;
  for (;;) {
    if (!drained) {
      int received = ReceiveStatus(child);
      if (received < 0) {
        if (received == -1 || (received == -3 && !child->forced)) child->cleanup = -1;
        drained = true;
      }
      else if (received > 0) continue;
    }
    int exited = alloy_pinned_exited(child->pidfd, 0);
    if (exited < 0) { child->cleanup = -1; break; }
    if (exited) {
      // Drain queued receipts after the exit barrier; never wait for socket EOF.
      while (!drained) {
        int received = ReceiveStatus(child);
        if (received <= 0) {
          if (received == -1 || (received == -3 && !child->forced)) child->cleanup = -1;
          drained = true;
        }
      }
      break;
    }
    pollfd events[] = {{drained ? -1 : child->status_channel, POLLIN, 0}, {child->pidfd, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 2, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0) { child->cleanup = -1; break; }
  }
  if (child->runtime_pidfd >= 0 && alloy_pinned_exited(child->runtime_pidfd, 1) != 1) child->cleanup = -1;
  if (child->probe_pidfd >= 0 && alloy_pinned_exited(child->probe_pidfd, 1) != 1) child->cleanup = -1;
  int status = -1;
  int waited = alloy_pinned_wait(child->pidfd, &status);
  bool evidence = child->finished || child->forced;
  if (waited && !(waited == ECHILD && evidence)) child->cleanup = -1;
  if (!child->forced && (!child->finished || child->receipt_status != 0)) child->cleanup = -1;
  if (child->forced && !waited) {
    bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    bool already_clean = status == 0 && child->finished && child->receipt_status == 0;
    if (!killed && !already_clean) child->cleanup = -1;
  }
  child->process = -1;
  SupervisorForkLock guard;
  for (int* fd : {&child->pidfd, &child->status_channel, &child->exec_error, &child->runtime_pidfd,
                 &child->probe_pidfd}) {
    if (*fd >= 0) {
      if (close(*fd)) child->cleanup = -1;
      *fd = -1;
    }
  }
  return child->cleanup;
}
}  // namespace alloy::guest
