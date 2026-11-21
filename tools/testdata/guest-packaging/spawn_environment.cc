// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Exercise the actual pinned spawn delegate. The portal/helper are test sinks,
// not processes: ownership must already be correct at serialization admission.
#include "preload/host/spawn_strategy/spawn_launcher_delegate.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace {
std::unordered_map<std::string, std::string> captured;
constexpr char kBootstrap[] = "/app/lib/alloy/alloy_guest_host";
}

namespace zypak {
std::vector<std::string> Launcher::Helper::BuildCommandWrapper(const FdMap&) const {
  return {"fixture-helper", "child", "-"};
}
namespace dbus {
void FlatpakPortalProxy::SpawnOptions::ExposePathRo(cstring_view) {
  assert(false && "This fixture transfers only the RAM directory capability");
}
void FlatpakPortalProxy::SpawnAsync(SpawnCall spawn, SpawnReplyHandler handler) {
  assert(spawn.fds && spawn.fds->size() == 2);
  assert(spawn.options.sandbox_expose_rw.size() == 1);
  captured = std::move(spawn.env);
  handler(std::uint32_t{1});
}
}  // namespace dbus
}  // namespace zypak

int main(int argc, char** argv) {
  assert(argc == 2);
  std::string cef = std::string(argv[1]) + "/cef";
  assert(!setenv("ALLOY_GUEST_RUNTIME", "1", 1));
  assert(!setenv("ALLOY_GUEST_BOOTSTRAP", kBootstrap, 1));
  assert(!setenv("ALLOY_GUEST_CEF_ROOT", cef.c_str(), 1));
  zypak::FdMap fds;
  fds.emplace_back(zypak::unique_fd(open("/dev/null", O_RDONLY | O_CLOEXEC)), 42);
  zypak::dbus::FlatpakPortalProxy portal;
  bool called = false;
  zypak::preload::SpawnLauncherDelegate delegate(&portal,
      [&called](zypak::dbus::FlatpakPortalProxy::SpawnReply) { called = true; });
  zypak::Launcher::Helper helper("fixture-helper", "zygote");
  assert(delegate.Spawn(helper, {kBootstrap, "--type=zygote"}, fds,
      {{"ALLOY_GUEST_BOOTSTRAP", "old-input"}, {"SBX_PID_NS", "1"}}, {},
      zypak::Launcher::Flags::kSandbox));
  assert(called);
  assert(captured.at("ALLOY_GUEST_RUNTIME") == "1");
  assert(captured.at("ALLOY_GUEST_BOOTSTRAP") == kBootstrap);
  assert(captured.at("ALLOY_GUEST_SCRATCH_FD") == "256");
  assert(captured.at("ALLOY_GUEST_PRESERVE_FDS") == "42");
  assert(captured.at("SBX_PID_NS") == "1");
  puts("ZYPAK_SPAWN_ENV_OWNED=1");
}
