// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "build/build_config.h"
#include "build/config/linux/dbus/buildflags.h"

#if BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/supports_user_data.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/dbus/thread_linux/dbus_thread_linux.h"
#include "content/public/test/browser_task_environment.h"
#include "dbus/bus.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {
class LateBusData final : public base::SupportsUserData::Data {
 public:
  explicit LateBusData(base::OnceClosure destroyed)
      : destroyed_(std::move(destroyed)) {}
  ~LateBusData() override {
    // Real keyed-service destructors can first obtain a shared bus here. Do
    // this from context destruction, not the test body, to cover that boundary.
    EXPECT_TRUE(dbus_thread_linux::GetSharedSessionBus());
    EXPECT_TRUE(dbus_thread_linux::GetSharedSystemBus());
    std::move(destroyed_).Run();
  }
 private:
  base::OnceClosure destroyed_;
};

class LifecycleHarness final : public ChromeRenderViewHostTestHarness {
 public:
  using ChromeRenderViewHostTestHarness::SetUp;
  using ChromeRenderViewHostTestHarness::TearDown;
 private:
  void TestBody() override {}
};

TEST(NativeTestLifetime, LateContextTeardownDoesNotOutliveItsPool) {
  static const int key = 0;
  // Each harness owns a fresh task environment. An unclosed bus from the first
  // context would post through its retired task tracker in the second scope.
  for (int lifetime = 0; lifetime < 2; ++lifetime) {
    bool destroyed = false;
    {
      LifecycleHarness fixture;
      base::ScopedClosureRunner teardown(base::BindOnce(
          &LifecycleHarness::TearDown, base::Unretained(&fixture)));
      ASSERT_NO_FATAL_FAILURE(fixture.SetUp());
      fixture.profile()->SetUserData(&key, std::make_unique<LateBusData>(
          base::BindOnce([](bool* completed) { *completed = true; }, &destroyed)));
    }
    EXPECT_TRUE(destroyed) << lifetime;
  }
}

TEST(NativeTestLifetime, DirectEnvironmentAndHarnessShareTheBusCleanupOwner) {
  static const int key = 0;
  for (int lifetime = 0; lifetime < 2; ++lifetime) {
    bool destroyed = false;
    {
      content::BrowserTaskEnvironment environment;
      auto profile = TestingProfile::Builder().Build();
      profile->SetUserData(&key, std::make_unique<LateBusData>(
          base::BindOnce([](bool* completed) { *completed = true; }, &destroyed)));
      base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(
          FROM_HERE, std::move(profile));
    }
    EXPECT_TRUE(destroyed) << lifetime;
    // A following harness must not inherit a bus tied to the direct
    // environment's retired pool, regardless of other tests' initialization.
    LifecycleHarness fixture;
    base::ScopedClosureRunner teardown(base::BindOnce(
        &LifecycleHarness::TearDown, base::Unretained(&fixture)));
    ASSERT_NO_FATAL_FAILURE(fixture.SetUp());
  }
}
}  // namespace
#endif
