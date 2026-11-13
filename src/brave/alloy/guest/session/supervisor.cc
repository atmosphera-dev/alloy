// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/supervisor.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
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

bool LaunchSupervisor(Supervisor* child) {
  std::string bootstrap = BundledBootstrap();
  if (bootstrap.empty() || access(bootstrap.c_str(), X_OK))
    return false;
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
    return false;
  int resources[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, resources)) {
    close(pair[0]);
    close(pair[1]);
    return false;
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
  char role[] = "--alloy-supervisor";
  char* argv[] = {bootstrap.data(), role, nullptr};
  posix_spawn_file_actions_t actions;
  int setup = posix_spawn_file_actions_init(&actions);
  if (setup) {
    close(pair[0]);
    close(pair[1]);
    close(resources[0]);
    close(resources[1]);
    return false;
  }
  setup = posix_spawn_file_actions_adddup2(&actions, pair[1],
                                          ALLOY_GUEST_CONTROL_FD);
  if (!setup)
    setup = posix_spawn_file_actions_adddup2(&actions, resources[1],
                                            ALLOY_GUEST_RESOURCE_FD);
  pid_t process = -1;
  int result = setup ? setup : posix_spawn(&process, bootstrap.c_str(), &actions,
                                          nullptr, argv, env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(pair[1]);
  close(resources[1]);
  if (result) {
    close(pair[0]);
    close(resources[0]);
    return false;
  }
  child->process = process;
  child->channel = pair[0];
  child->resources = resources[0];
  child->pidfd = static_cast<int>(syscall(SYS_pidfd_open, process, 0));
  if (child->pidfd < 0 ||
      fcntl(child->channel, F_SETFL, O_NONBLOCK) < 0) {
    ReapSupervisor(child);
    return false;
  }
  return true;
}

void ReapSupervisor(Supervisor* child) {
  if (child->resources >= 0) {
    close(child->resources);
    child->resources = -1;
  }
  if (child->channel >= 0) {
    close(child->channel);
    child->channel = -1;
  }
  if (child->process > 0) {
    while (waitpid(child->process, nullptr, 0) < 0 && errno == EINTR) {}
    child->process = -1;
  }
  if (child->pidfd >= 0) {
    close(child->pidfd);
    child->pidfd = -1;
  }
}
}  // namespace alloy::guest
