// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/linux_lock_signals.h"

#include <array>
#include <algorithm>
#include <unistd.h>
#include "base/functional/bind.h"
#include "dbus/message.h"

namespace alloy::site_records {
namespace {
constexpr char kLogind[] = "org.freedesktop.login1";
constexpr char kManager[] = "org.freedesktop.login1.Manager";
constexpr char kSession[] = "org.freedesktop.login1.Session";
constexpr char kProperties[] = "org.freedesktop.DBus.Properties";
struct ScreenProvider { const char* name; const char* path; };
constexpr auto kScreens = std::to_array<ScreenProvider>({
    {"org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver"},
    {"org.gnome.ScreenSaver", "/org/gnome/ScreenSaver"},
    {"org.cinnamon.ScreenSaver", "/org/cinnamon/ScreenSaver"},
    {"org.mate.ScreenSaver", "/org/mate/ScreenSaver"},
    {"org.xfce.ScreenSaver", "/org/xfce/ScreenSaver"}});
}

LinuxLockSignals::LinuxLockSignals(scoped_refptr<dbus::Bus> system,
    scoped_refptr<dbus::Bus> session, ProfileAutoLock::RequestLock signal,
    Availability availability)
    : system_(std::move(system)), session_(std::move(session)),
      signal_(std::move(signal)), availability_(std::move(availability)),
      screen_connected_(kScreens.size(), false), screen_available_(kScreens.size(), false),
      screen_requests_(kScreens.size(), 0) {}
LinuxLockSignals::~LinuxLockSignals() = default;
void LinuxLockSignals::SetAvailable(ProfileLockReason reason, bool available) {
  const int bit = 1 << static_cast<int>(reason);
  const int next = available ? available_ | bit : available_ & ~bit;
  if (next != available_) { available_ = next; availability_.Run(next); }
}
void LinuxLockSignals::Start() {
  manager_ = system_->GetObjectProxy(kLogind, dbus::ObjectPath("/org/freedesktop/login1"));
  manager_->SetNameOwnerChangedCallback(base::BindRepeating(
      &LinuxLockSignals::LogindOwner, weak_factory_.GetWeakPtr()));
  manager_->ConnectToSignal(kManager, "PrepareForSleep",
      base::BindRepeating(&LinuxLockSignals::Sleep, weak_factory_.GetWeakPtr()),
      base::BindOnce([](base::WeakPtr<LinuxLockSignals> self, const std::string&,
                        const std::string&, bool ok) {
        if (!self) return;
        self->sleep_connected_ = ok;
        if (ok) self->DiscoverLogind();
      }, weak_factory_.GetWeakPtr()));
  manager_->ConnectToSignal(kManager, "SessionRemoved",
      base::BindRepeating(&LinuxLockSignals::Removed, weak_factory_.GetWeakPtr()),
      base::BindOnce([](const std::string&, const std::string&, bool) {}));
  for (size_t i = 0; i < kScreens.size(); ++i) {
    auto* proxy = session_->GetObjectProxy(kScreens[i].name, dbus::ObjectPath(kScreens[i].path));
    proxy->SetNameOwnerChangedCallback(base::BindRepeating(
        [](base::WeakPtr<LinuxLockSignals> self, size_t i, const std::string&, const std::string&) {
          if (!self) return;
          self->screen_available_[i] = false;
          self->SetAvailable(ProfileLockReason::kScreenLock, self->logind_lock_ ||
              std::ranges::any_of(self->screen_available_, [](bool v) { return v; }));
          self->ReadScreen(i);
        }, weak_factory_.GetWeakPtr(), i));
    proxy->ConnectToSignal(kScreens[i].name, "ActiveChanged",
        base::BindRepeating(&LinuxLockSignals::Screen, weak_factory_.GetWeakPtr(), i),
        base::BindOnce([](base::WeakPtr<LinuxLockSignals> self, size_t i,
                          const std::string&, const std::string&, bool ok) {
          if (!self) return;
          self->screen_connected_[i] = ok;
          self->ReadScreen(i);
        }, weak_factory_.GetWeakPtr(), i));
  }
}
void LinuxLockSignals::DiscoverLogind() {
  // Subscribe before querying. A sleep in progress at startup also locks.
  dbus::MethodCall state(kProperties, "Get");
  dbus::MessageWriter writer(&state);
  writer.AppendString(kManager); writer.AppendString("PreparingForSleep");
  dbus_message_set_auto_start(state.raw_message(), false);
  manager_->CallMethod(&state, 3000, base::BindOnce(
      [](base::WeakPtr<LinuxLockSignals> self, dbus::Response* response) {
        if (!self) return;
        bool preparing = false;
        dbus::MessageReader reader(response);
        const bool ok = response && reader.PopVariantOfBool(&preparing);
        self->SetAvailable(ProfileLockReason::kSleep, ok && self->sleep_connected_);
        if (ok && preparing) self->signal_.Run(ProfileLockReason::kSleep);
      }, logind_requests_.GetWeakPtr()));
  dbus::MethodCall method(kManager, "GetSessionByPID");
  dbus::MessageWriter(&method).AppendUint32(getpid());
  dbus_message_set_auto_start(method.raw_message(), false);
  manager_->CallMethod(&method, 3000, base::BindOnce(
      &LinuxLockSignals::SessionFound, logind_requests_.GetWeakPtr()));
}
void LinuxLockSignals::SessionFound(dbus::Response* response) {
  dbus::ObjectPath path;
  if (!response) return;
  dbus::MessageReader reader(response);
  if (!reader.PopObjectPath(&path)) return;
  session_path_ = path.value();
  own_session_ = system_->GetObjectProxy(kLogind, path);
  own_session_->ConnectToSignal(kSession, "Lock", base::BindRepeating(
      [](base::WeakPtr<LinuxLockSignals> self, dbus::Signal*) {
        if (self) self->signal_.Run(ProfileLockReason::kScreenLock);
      }, logind_requests_.GetWeakPtr()),
      base::BindOnce([](const std::string&, const std::string&, bool) {}));
  own_session_->ConnectToSignal(kProperties, "PropertiesChanged",
      base::BindRepeating(&LinuxLockSignals::PropertyChanged, logind_requests_.GetWeakPtr()),
      base::BindOnce([](base::WeakPtr<LinuxLockSignals> self, const std::string&,
                        const std::string&, bool ok) {
        if (!self) return;
        self->session_connected_ = ok;
        if (ok) self->ReadSession();
      }, logind_requests_.GetWeakPtr()));
}
void LinuxLockSignals::ReadSession() {
  if (!own_session_) return;
  dbus::MethodCall method(kProperties, "GetAll");
  dbus::MessageWriter(&method).AppendString(kSession);
  dbus_message_set_auto_start(method.raw_message(), false);
  own_session_->CallMethod(&method, 3000, base::BindOnce(
      &LinuxLockSignals::SessionProperties, logind_requests_.GetWeakPtr()));
}
void LinuxLockSignals::SessionProperties(dbus::Response* response) {
  if (!response) {
    logind_lock_ = false;
    SetAvailable(ProfileLockReason::kUserSwitch, false);
    SetAvailable(ProfileLockReason::kScreenLock,
        std::ranges::any_of(screen_available_, [](bool v) { return v; }));
    return;
  }
  dbus::MessageReader reader(response), properties(nullptr);
  if (reader.PopArray(&properties)) ApplyProperties(properties);
}
void LinuxLockSignals::ApplyProperties(dbus::MessageReader& properties) {
  while (properties.HasMoreData()) {
    dbus::MessageReader entry(nullptr); std::string name; bool value;
    if (!properties.PopDictEntry(&entry) || !entry.PopString(&name)) return;
    if (name != "LockedHint" && name != "Active") continue;
    if (!entry.PopVariantOfBool(&value)) continue;
    if (name == "Active") {
      SetAvailable(ProfileLockReason::kUserSwitch, session_connected_);
      if (!value) signal_.Run(ProfileLockReason::kUserSwitch);
    } else {
      logind_lock_ = session_connected_;
      SetAvailable(ProfileLockReason::kScreenLock, logind_lock_ ||
          std::ranges::any_of(screen_available_, [](bool v) { return v; }));
      if (value) signal_.Run(ProfileLockReason::kScreenLock);
    }
  }
}
void LinuxLockSignals::PropertyChanged(dbus::Signal* signal) {
  dbus::MessageReader reader(signal), properties(nullptr);
  std::string interface;
  if (!reader.PopString(&interface) || interface != kSession || !reader.PopArray(&properties)) return;
  ApplyProperties(properties);
  std::vector<std::string> invalidated;
  if (reader.PopArrayOfStrings(&invalidated) && !invalidated.empty()) ReadSession();
}
void LinuxLockSignals::Sleep(dbus::Signal* signal) {
  bool preparing;
  dbus::MessageReader reader(signal);
  if (reader.PopBool(&preparing) && preparing) signal_.Run(ProfileLockReason::kSleep);
}
void LinuxLockSignals::Removed(dbus::Signal* signal) {
  std::string id; dbus::ObjectPath path;
  dbus::MessageReader reader(signal);
  if (reader.PopString(&id) && reader.PopObjectPath(&path) && path.value() == session_path_)
    signal_.Run(ProfileLockReason::kUserSwitch);
}
void LinuxLockSignals::ReadScreen(size_t index) {
  const auto request = ++screen_requests_[index];
  dbus::MethodCall method(kScreens[index].name, "GetActive");
  dbus_message_set_auto_start(method.raw_message(), false);
  session_->GetObjectProxy(kScreens[index].name, dbus::ObjectPath(kScreens[index].path))->CallMethod(
      &method, 3000, base::BindOnce([](base::WeakPtr<LinuxLockSignals> self, size_t i,
                                     uint64_t request, dbus::Response* response) {
        if (!self || self->screen_requests_[i] != request) return;
        bool active = false;
        dbus::MessageReader reader(response);
        const bool ok = response && reader.PopBool(&active);
        self->screen_available_[i] = ok && self->screen_connected_[i];
        self->SetAvailable(ProfileLockReason::kScreenLock, self->logind_lock_ ||
            std::ranges::any_of(self->screen_available_, [](bool v) { return v; }));
        if (ok && active) self->signal_.Run(ProfileLockReason::kScreenLock);
      }, weak_factory_.GetWeakPtr(), index, request));
}
void LinuxLockSignals::Screen(size_t index, dbus::Signal* signal) {
  bool active; dbus::MessageReader reader(signal);
  if (screen_connected_[index] && reader.PopBool(&active) && active)
    signal_.Run(ProfileLockReason::kScreenLock);
}
void LinuxLockSignals::LogindOwner(const std::string&, const std::string& after) {
  // Outstanding replies and own-session subscriptions belong to the original
  // service owner. They must not resurrect availability after owner loss.
  logind_requests_.InvalidateWeakPtrs();
  logind_lock_ = false; own_session_ = nullptr; session_path_.clear();
  SetAvailable(ProfileLockReason::kSleep, false);
  SetAvailable(ProfileLockReason::kUserSwitch, false);
  SetAvailable(ProfileLockReason::kScreenLock,
      std::ranges::any_of(screen_available_, [](bool v) { return v; }));
  if (!after.empty()) DiscoverLogind();
}

}  // namespace alloy::site_records
