// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/linux_lock_signals.h"

#include <map>
#include <functional>
#include <unistd.h>
#include <vector>
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "dbus/message.h"
#include "dbus/mock_bus.h"
#include "dbus/mock_object_proxy.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
using testing::_;
using testing::NiceMock;
constexpr char kManager[] = "/org/freedesktop/login1";
constexpr char kOwn[] = "/org/freedesktop/login1/session/own";
constexpr char kProperties[] = "org.freedesktop.DBus.Properties";
constexpr char kSession[] = "org.freedesktop.login1.Session";

class LinuxLockSignalsTest : public testing::Test {
 protected:
  void SetUp() override {
    bus_ = base::MakeRefCounted<NiceMock<dbus::MockBus>>(dbus::Bus::Options());
    ON_CALL(*bus_, GetObjectProxy(_, _)).WillByDefault([&](std::string_view name, const dbus::ObjectPath& path) {
      const auto key = std::string(name)+path.value();
      auto& proxy = proxies_[key];
      if (!proxy) {
        proxy = base::MakeRefCounted<NiceMock<dbus::MockObjectProxy>>(bus_.get(), std::string(name), path);
        ON_CALL(*proxy, SetNameOwnerChangedCallback(_)).WillByDefault([&, key](auto cb) { owners_[key] = std::move(cb); });
        ON_CALL(*proxy, ConnectToSignal(_, _, _, _)).WillByDefault(
            [&, key](const std::string& iface, const std::string& signal, auto cb, auto connected) {
              signals_[key+iface+signal] = std::move(cb); std::move(connected).Run(iface, signal, true);
            });
        ON_CALL(*proxy, CallMethod(_, _, _)).WillByDefault(
            [&](dbus::MethodCall* call, int timeout, auto done) {
              EXPECT_EQ(3000, timeout);
              EXPECT_FALSE(dbus_message_get_auto_start(call->raw_message()));
              if (hold_reads_ && (call->GetMember() == "Get" || call->GetMember() == "GetAll")) {
                held_reads_.push_back(std::move(done)); return;
              }
              auto response = dbus::Response::CreateEmpty();
              dbus::MessageWriter writer(response.get());
              if (missing_) { std::move(done).Run(nullptr); return; }
              if (call->GetMember() == "GetSessionByPID") {
                uint32_t pid; dbus::MessageReader reader(call); EXPECT_TRUE(reader.PopUint32(&pid));
                EXPECT_EQ(static_cast<uint32_t>(getpid()), pid); writer.AppendObjectPath(dbus::ObjectPath(kOwn));
              } else if (call->GetMember() == "GetAll") {
                dbus::MessageWriter properties(nullptr); writer.OpenArray("{sv}", &properties);
                Property(properties, "LockedHint", initially_locked_);
                Property(properties, "Active", !initially_inactive_);
                writer.CloseContainer(&properties);
              } else if (call->GetMember() == "Get") writer.AppendVariantOfBool(initially_sleeping_);
              else { std::move(done).Run(nullptr); return; }
              std::move(done).Run(response.get());
            });
      }
      return proxy.get();
    });
  }
  static void Property(dbus::MessageWriter& writer, const char* name, bool value) {
    dbus::MessageWriter entry(nullptr); writer.OpenDictEntry(&entry);
    entry.AppendString(name); entry.AppendVariantOfBool(value); writer.CloseContainer(&entry);
  }
  void Start() {
    source_ = std::make_unique<LinuxLockSignals>(bus_, bus_, base::BindLambdaForTesting(
        [&](ProfileLockReason reason) { reasons_.push_back(reason); }),
        base::BindLambdaForTesting([&](int mask) { masks_.push_back(mask); }));
    source_->Start();
  }
  void Emit(const char* path, const char* iface, const char* member, std::function<void(dbus::MessageWriter&)> write) {
    dbus::Signal signal(iface, member); dbus::MessageWriter writer(&signal); write(writer);
    const auto key = std::string("org.freedesktop.login1")+path+iface+member;
    ASSERT_TRUE(signals_.contains(key)); signals_[key].Run(&signal);
  }
  void Changed(const char* interface, const char* property, bool value) {
    Emit(kOwn, kProperties, "PropertiesChanged", [&](auto& writer) {
      writer.AppendString(interface); dbus::MessageWriter props(nullptr); writer.OpenArray("{sv}", &props);
      Property(props, property, value); writer.CloseContainer(&props);
      writer.AppendArrayOfStrings({});
    });
  }
  base::test::TaskEnvironment tasks_;
  scoped_refptr<NiceMock<dbus::MockBus>> bus_;
  std::map<std::string, scoped_refptr<NiceMock<dbus::MockObjectProxy>>> proxies_;
  std::map<std::string, dbus::ObjectProxy::SignalCallback> signals_;
  std::map<std::string, dbus::ObjectProxy::NameOwnerChangedCallback> owners_;
  std::vector<ProfileLockReason> reasons_;
  std::vector<int> masks_;
  bool missing_ = false, initially_locked_ = false, initially_inactive_ = false, initially_sleeping_ = false;
  bool hold_reads_ = false;
  std::vector<dbus::ObjectProxy::ResponseCallback> held_reads_;
  std::unique_ptr<LinuxLockSignals> source_;
};

TEST_F(LinuxLockSignalsTest, OwnSessionSignalsIgnoreResumeUnlockActiveAndOtherInterfaces) {
  Start();
  EXPECT_EQ((1 << 3) | (1 << 4) | (1 << 5), source_->available());
  EXPECT_TRUE(reasons_.empty());
  Emit(kManager, "org.freedesktop.login1.Manager", "PrepareForSleep", [](auto& w) { w.AppendBool(false); });
  Changed(kSession, "LockedHint", false); Changed(kSession, "Active", true);
  Changed("org.unrelated", "Active", false); EXPECT_TRUE(reasons_.empty());
  Emit(kManager, "org.freedesktop.login1.Manager", "PrepareForSleep", [](auto& w) { w.AppendBool(true); });
  Emit(kOwn, kSession, "Lock", [](auto&) {});
  Changed(kSession, "LockedHint", true); Changed(kSession, "Active", false);
  ASSERT_EQ(4u, reasons_.size());
  EXPECT_EQ(ProfileLockReason::kSleep, reasons_[0]);
  EXPECT_EQ(ProfileLockReason::kScreenLock, reasons_[1]);
  EXPECT_EQ(ProfileLockReason::kScreenLock, reasons_[2]);
  EXPECT_EQ(ProfileLockReason::kUserSwitch, reasons_[3]);
}
TEST_F(LinuxLockSignalsTest, InitialStateAndProviderLossAreNotTreatedAsActivity) {
  initially_locked_ = initially_inactive_ = initially_sleeping_ = true;
  Start(); ASSERT_EQ(3u, reasons_.size());
  missing_ = true;
  owners_[std::string("org.freedesktop.login1")+kManager].Run(":1.1", "");
  EXPECT_EQ(0, source_->available());
  EXPECT_EQ(3u, reasons_.size());
  missing_ = false;
  owners_[std::string("org.freedesktop.login1")+kManager].Run("", ":1.2");
  EXPECT_EQ((1 << 3) | (1 << 4) | (1 << 5), source_->available());
  EXPECT_EQ(6u, reasons_.size());
}
TEST_F(LinuxLockSignalsTest, MissingProvidersAndDestroyedObserversDoNotFakeAvailability) {
  missing_ = true; Start(); EXPECT_EQ(0, source_->available()); EXPECT_TRUE(reasons_.empty());
  source_.reset();
  Emit(kManager, "org.freedesktop.login1.Manager", "PrepareForSleep", [](auto& w) { w.AppendBool(true); });
  EXPECT_TRUE(reasons_.empty());
}
TEST_F(LinuxLockSignalsTest, OnlyOwnSessionRemovalLocks) {
  Start();
  Emit(kManager, "org.freedesktop.login1.Manager", "SessionRemoved", [](auto& w) {
    w.AppendString("other"); w.AppendObjectPath(dbus::ObjectPath("/org/freedesktop/login1/session/other"));
  });
  EXPECT_TRUE(reasons_.empty());
  Emit(kManager, "org.freedesktop.login1.Manager", "SessionRemoved", [](auto& w) {
    w.AppendString("own"); w.AppendObjectPath(dbus::ObjectPath(kOwn));
  });
  ASSERT_EQ(1u, reasons_.size()); EXPECT_EQ(ProfileLockReason::kUserSwitch, reasons_[0]);
}
TEST_F(LinuxLockSignalsTest, DelayedRepliesCannotResurrectDepartedProviderOrSession) {
  hold_reads_ = true; Start();
  ASSERT_EQ(2u, held_reads_.size()); EXPECT_EQ(0, source_->available());
  owners_[std::string("org.freedesktop.login1")+kManager].Run(":1.1", "");
  auto sleep = dbus::Response::CreateEmpty();
  dbus::MessageWriter(sleep.get()).AppendVariantOfBool(true);
  std::move(held_reads_[0]).Run(sleep.get());
  auto session = dbus::Response::CreateEmpty(); dbus::MessageWriter writer(session.get()), props(nullptr);
  writer.OpenArray("{sv}", &props); Property(props, "LockedHint", true); Property(props, "Active", false);
  writer.CloseContainer(&props); std::move(held_reads_[1]).Run(session.get());
  EXPECT_EQ(0, source_->available()); EXPECT_TRUE(reasons_.empty());
}
}  // namespace
}  // namespace alloy::site_records
