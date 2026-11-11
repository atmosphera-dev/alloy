// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_LINUX_LOCK_SIGNALS_H_
#define BRAVE_ALLOY_SITE_RECORDS_LINUX_LOCK_SIGNALS_H_

#include <memory>
#include <string>
#include <vector>
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "brave/alloy/site_records/profile_auto_lock.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_proxy.h"

namespace alloy::site_records {

// A missing provider is a capability change, not evidence of activity/unlock.
// Injected buses exercise exactly the production subscription/parser path.
class LinuxLockSignals final {
 public:
  using Availability = base::RepeatingCallback<void(int)>;
  LinuxLockSignals(scoped_refptr<dbus::Bus> system, scoped_refptr<dbus::Bus> session,
                   ProfileAutoLock::RequestLock signal, Availability availability);
  ~LinuxLockSignals();
  void Start();
  int available() const { return available_; }

 private:
  void SetAvailable(ProfileLockReason reason, bool available);
  void DiscoverLogind();
  void SessionFound(dbus::Response* response);
  void ReadSession();
  void SessionProperties(dbus::Response* response);
  void PropertyChanged(dbus::Signal* signal);
  void ApplyProperties(dbus::MessageReader& properties);
  void Sleep(dbus::Signal* signal);
  void Removed(dbus::Signal* signal);
  void Screen(size_t index, dbus::Signal* signal);
  void ReadScreen(size_t index);
  void LogindOwner(const std::string& before, const std::string& after);
  const scoped_refptr<dbus::Bus> system_, session_;
  const ProfileAutoLock::RequestLock signal_;
  const Availability availability_;
  raw_ptr<dbus::ObjectProxy> manager_ = nullptr;
  raw_ptr<dbus::ObjectProxy> own_session_ = nullptr;
  std::string session_path_;
  bool sleep_connected_ = false, session_connected_ = false;
  bool logind_lock_ = false;
  std::vector<bool> screen_connected_, screen_available_;
  std::vector<uint64_t> screen_requests_;
  int available_ = 0;
  base::WeakPtrFactory<LinuxLockSignals> logind_requests_{this};
  base::WeakPtrFactory<LinuxLockSignals> weak_factory_{this};
};
}  // namespace alloy::site_records
#endif
