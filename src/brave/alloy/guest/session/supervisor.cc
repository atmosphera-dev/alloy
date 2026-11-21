// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/supervisor.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "base/process/alloy_guest_runtime.h"

namespace alloy::guest {
std::string BundledBootstrap() {
  Dl_info info = {};
  char path[PATH_MAX];
  if (!dladdr(reinterpret_cast<void*>(&BundledBootstrap), &info) ||
      !info.dli_fname || !realpath(info.dli_fname, path))
    return {};
  std::string library(path);
  auto separator = library.find_last_of('/');
  if (separator == std::string::npos)
    return {};
  return library.substr(0, separator + 1) + "alloy_guest_host";
}

namespace {
bool ReceiveStatus(Supervisor* child, alloy_guest_wait_status* packet) {
  iovec io = {packet, sizeof(*packet)};
  union { cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control = {};
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  ssize_t count;
  do { count = recvmsg(child->status_channel, &message, MSG_CMSG_CLOEXEC); }
  while (count < 0 && errno == EINTR);
  bool valid = count == sizeof(*packet) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
               packet->magic == ALLOY_GUEST_WAIT_STATUS_MAGIC;
  int descriptor = -1;
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
      size_t size = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      auto* handles = reinterpret_cast<int*>(CMSG_DATA(header));
      for (size_t i = 0; i < size; ++i) {
        if (descriptor < 0) descriptor = handles[i];
        else { close(handles[i]); valid = false; }
      }
    } else {
      valid = false;
    }
  }
  bool transfer = packet->phase == ALLOY_GUEST_WAIT_READY && !packet->error;
  valid = valid && (packet->phase == ALLOY_GUEST_WAIT_READY || packet->phase == ALLOY_GUEST_WAIT_FINISHED) &&
          (transfer ? descriptor >= 0 && child->runtime_pidfd < 0 : descriptor < 0);
  if (valid && transfer) child->runtime_pidfd = descriptor;
  else if (descriptor >= 0) close(descriptor);
  return valid;
}

bool SpawnBootstrap(const std::string& bootstrap, const char* role,
                    Supervisor* child, int* identity_error = nullptr) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
    return false;
  int resources[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, resources)) {
    close(pair[0]);
    close(pair[1]);
    return false;
  }
  int status[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, status)) {
    close(pair[0]); close(pair[1]); close(resources[0]); close(resources[1]);
    return false;
  }
  // dup2 actions execute in order. Neither source may alias either target,
  // including when the embedding app already has many descriptors open.
  for (int* source : {&pair[1], &resources[1], &status[1]}) {
    if (*source >= ALLOY_GUEST_WAIT_STATUS_FD && *source <= ALLOY_GUEST_RESOURCE_FD) {
      int moved = fcntl(*source, F_DUPFD_CLOEXEC, ALLOY_GUEST_RESOURCE_FD + 1);
      if (moved < 0) {
        close(pair[0]);
        close(pair[1]);
        close(resources[0]);
        close(resources[1]);
        close(status[0]); close(status[1]);
        return false;
      }
      close(*source);
      *source = moved;
    }
  }
  std::vector<std::string> environment = {
      "PATH=/usr/bin:/bin", "LANG=C.UTF-8", "ALLOY_GUEST_RUNTIME=1",
      "ALLOY_GUEST_BOOTSTRAP=" + bootstrap,
      "GSETTINGS_BACKEND=memory", "GIO_USE_VFS=local",
      "DBUS_SESSION_BUS_ADDRESS=disabled:", "NO_AT_BRIDGE=1",
      "MESA_SHADER_CACHE_DISABLE=true", "__GL_SHADER_DISK_CACHE=0"};
  // Explicit native path inputs only. Never copy loader, plugin, graphics,
  // credential, desktop-bus, display or audio environment wholesale. M2 owns
  // verified RAM redirection; these inputs make no no-disk claim in M1.
  for (const char* name : {"HOME", "TMPDIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                          "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"}) {
    const char* value = getenv(name);
    if (value)
      environment.emplace_back(std::string(name) + "=" + value);
  }
  std::vector<char*> env;
  for (auto& field : environment)
    env.push_back(field.data());
  env.push_back(nullptr);
  std::string executable = bootstrap;
  std::string argument = role;
  char* argv[] = {executable.data(), argument.data(), nullptr};
  posix_spawn_file_actions_t actions;
  int setup = posix_spawn_file_actions_init(&actions);
  if (setup) {
    close(pair[0]);
    close(pair[1]);
    close(resources[0]);
    close(resources[1]);
    close(status[0]); close(status[1]);
    return false;
  }
  setup = posix_spawn_file_actions_adddup2(&actions, pair[1],
                                          ALLOY_GUEST_CONTROL_FD);
  if (!setup)
    setup = posix_spawn_file_actions_adddup2(&actions, resources[1],
                                            ALLOY_GUEST_RESOURCE_FD);
  if (!setup)
    setup = posix_spawn_file_actions_adddup2(&actions, status[1], ALLOY_GUEST_WAIT_STATUS_FD);
  pid_t process = -1;
  int result = setup ? setup : posix_spawn(&process, bootstrap.c_str(), &actions,
                                          nullptr, argv, env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(pair[1]);
  close(resources[1]);
  close(status[1]);
  if (result) {
    close(pair[0]);
    close(resources[0]);
    close(status[0]);
    return false;
  }
  child->process = process;
  child->channel = pair[0];
  child->resources = resources[0];
  child->status_channel = status[0];
  child->pidfd = static_cast<int>(syscall(SYS_pidfd_open, process, 0));
  if (child->pidfd < 0) {
    if (identity_error) *identity_error = errno;
    ReapSupervisor(child);
    return false;
  }
  return true;
}
}  // namespace

bool LaunchSupervisor(Supervisor* child, std::string* unavailable, int cancel) {
  unavailable->clear();
  std::string bootstrap = BundledBootstrap();
  if (bootstrap.empty() || access(bootstrap.c_str(), X_OK)) return false;
  int identity_error = 0;
  if (!SpawnBootstrap(bootstrap, ALLOY_GUEST_WAIT_OWNER_ARGUMENT, child, &identity_error)) {
    if (identity_error)
      *unavailable = std::string("identity-bound process teardown unavailable: pidfd_open: ") +
                     strerror(identity_error);
    return false;
  }
  ssize_t granted;
  do { granted = send(child->status_channel, "G", 1, MSG_NOSIGNAL); }
  while (granted < 0 && errno == EINTR);
  alloy_guest_wait_status packet = {};
  bool ready = false;
  while (granted == 1 && !ready) {
    pollfd events[] = {{child->status_channel, POLLIN, 0}, {child->pidfd, POLLIN, 0},
                       {cancel, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 3, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0 || events[2].revents) break;
    if (events[0].revents & POLLIN) {
      ready = ReceiveStatus(child, &packet) && packet.phase == ALLOY_GUEST_WAIT_READY;
      break;
    }
    if (events[0].revents || events[1].revents) break;
  }
  if (!ready || packet.error) {
    int error = ready ? packet.error : ECANCELED;
    ReapSupervisor(child);
    const char* operation = packet.operation == ALLOY_GUEST_WAIT_SIGNAL ? "pidfd_send_signal" :
                            packet.operation == ALLOY_GUEST_WAIT_PIDFD ? "pidfd_open" : "static wait owner";
    *unavailable = (error == EPERM || error == EACCES) && packet.operation == ALLOY_GUEST_WAIT_SIGNAL
        ? "process policy denies pidfd_send_signal: identity-bound process teardown unavailable"
        : std::string("identity-bound process teardown unavailable: ") + operation + ": " + strerror(error);
    return false;
  }
  if (packet.status < 0 || !WIFSIGNALED(packet.status) || WTERMSIG(packet.status) != SIGKILL) {
    *unavailable = "identity-bound process teardown unavailable: probe did not terminate by SIGKILL";
    ReapSupervisor(child);
    return false;
  }
  if (fcntl(child->channel, F_SETFL, O_NONBLOCK) < 0) {
    ReapSupervisor(child);
    return false;
  }
  ssize_t accepted;
  do { accepted = send(child->status_channel, "A", 1, MSG_NOSIGNAL); }
  while (accepted < 0 && errno == EINTR);
  if (accepted != 1) {
    ReapSupervisor(child);
    return false;
  }
  return true;
}

int ReapSupervisor(Supervisor* child) {
  int status = child->process > 0 ? -1 : 0;
  if (child->resources >= 0) {
    close(child->resources);
    child->resources = -1;
  }
  if (child->channel >= 0) {
    close(child->channel);
    child->channel = -1;
  }
  if (child->process > 0) {
    // Closing startup is a request, not abandonment. The static owner still
    // has its probe pidfd and owns the inner supervisor's wait status.
    if (child->status_channel >= 0) {
      // After admission the owner waits for runtime exit, not more commands.
      // An unread cancel packet makes its final close reset the receipt lane.
      // EOF cancels startup without queuing data or closing our receive side.
      int result;
      do { result = shutdown(child->status_channel, SHUT_WR); }
      while (result < 0 && errno == EINTR);
      for (;;) {
        alloy_guest_wait_status packet = {};
        if (!ReceiveStatus(child, &packet)) break;
        if (packet.phase == ALLOY_GUEST_WAIT_FINISHED) {
          status = packet.error ? -1 : packet.status;
          break;
        }
        if (packet.phase != ALLOY_GUEST_WAIT_READY) break;
      }
    }
    // Even loss of the outer owner cannot skip the inner runtime exit barrier.
    // It was transferred before the app acknowledged any runtime admission.
    if (child->runtime_pidfd >= 0) {
      pollfd exited = {child->runtime_pidfd, POLLIN, 0};
      int result;
      do { result = poll(&exited, 1, -1); } while (result < 0 && errno == EINTR);
      if (result != 1 || !(exited.revents & POLLIN)) status = -1;
    }
    // A receipt alone is not final exit. The app's reaper may consume its direct
      // child status, but cannot recycle this pinned original process identity.
    if (child->pidfd >= 0) {
      pollfd exited = {child->pidfd, POLLIN, 0};
      int result;
      do { result = poll(&exited, 1, -1); } while (result < 0 && errno == EINTR);
      if (result != 1 || !(exited.revents & POLLIN)) status = -1;
    } else {
      status = -1;
    }
    if (child->pidfd >= 0) {
      // Reap by the pinned identity too: a competing app reaper must not make
      // a reused numeric PID consume another app child's status. ECHILD is
      // irrelevant here; the private receipt and pidfd exits proved outcome.
      siginfo_t ignored = {};
      // Linux UAPI P_PIDFD is 3; Chromium's older libc sysroot lacks its enum.
      constexpr idtype_t kPidFd = static_cast<idtype_t>(3);
      while (waitid(kPidFd, child->pidfd, &ignored, WEXITED) < 0 && errno == EINTR) {}
    }
    child->process = -1;
  }
  if (child->pidfd >= 0) {
    close(child->pidfd);
    child->pidfd = -1;
  }
  if (child->status_channel >= 0) {
    close(child->status_channel);
    child->status_channel = -1;
  }
  if (child->runtime_pidfd >= 0) {
    close(child->runtime_pidfd);
    child->runtime_pidfd = -1;
  }
  return status;
}
}  // namespace alloy::guest
