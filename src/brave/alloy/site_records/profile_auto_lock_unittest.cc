// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_auto_lock.h"

#include <vector>
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {
TEST(ProfileAutoLockTest, MonotonicIdleIsResetOnlyByInputAndEachUnlock) {
  base::test::TaskEnvironment tasks(base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  auto generation = base::UnguessableToken::Create();
  std::vector<ProfileLockReason> reasons;
  ProfileAutoLock timer(base::BindLambdaForTesting([&] { return generation; }),
      base::BindLambdaForTesting([&](ProfileLockReason reason) { reasons.push_back(reason); generation = {}; }));
  timer.SetIdleTimeout(base::Seconds(2));
  tasks.FastForwardBy(base::Seconds(1)); timer.UserActivity();
  tasks.FastForwardBy(base::Milliseconds(1900)); EXPECT_TRUE(reasons.empty());
  tasks.FastForwardBy(base::Milliseconds(100));
  ASSERT_EQ(1u, reasons.size()); EXPECT_EQ(ProfileLockReason::kIdle, reasons[0]);
  tasks.FastForwardBy(base::Seconds(10)); EXPECT_EQ(1u, reasons.size());
  generation = base::UnguessableToken::Create();
  tasks.FastForwardBy(base::Seconds(2)); EXPECT_EQ(1u, reasons.size());
  tasks.FastForwardBy(base::Milliseconds(100)); EXPECT_EQ(2u, reasons.size());
}
TEST(ProfileAutoLockTest, DisabledIdleDoesNotDisableOtherTriggersOrUnlock) {
  base::test::TaskEnvironment tasks(base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  auto generation = base::UnguessableToken::Create();
  std::vector<ProfileLockReason> reasons;
  ProfileAutoLock timer(base::BindLambdaForTesting([&] { return generation; }),
      base::BindLambdaForTesting([&](ProfileLockReason reason) { reasons.push_back(reason); generation = {}; }));
  timer.SetIdleTimeout(base::TimeDelta()); tasks.FastForwardBy(base::Hours(1));
  EXPECT_TRUE(reasons.empty());
  for (auto reason : {ProfileLockReason::kSleep, ProfileLockReason::kScreenLock,
                      ProfileLockReason::kUserSwitch, ProfileLockReason::kBrowserClose}) timer.Signal(reason);
  ASSERT_EQ(4u, reasons.size());
  timer.UserActivity(); EXPECT_TRUE(generation.is_empty());
}
TEST(ProfileAutoLockTest, DestroyedTimerCannotDispatchOrRetainConsumer) {
  base::test::TaskEnvironment tasks(base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  int calls = 0;
  {
    auto generation = base::UnguessableToken::Create();
    ProfileAutoLock timer(base::BindLambdaForTesting([&] { return generation; }),
        base::BindLambdaForTesting([&](ProfileLockReason) { ++calls; }));
    timer.SetIdleTimeout(base::Seconds(1));
  }
  tasks.FastForwardBy(base::Seconds(2)); EXPECT_EQ(0, calls);
}
TEST(ProfileAutoLockTest, NewGenerationResetsIdleEvenWithoutAnObservedLockedTick) {
  base::test::TaskEnvironment tasks(base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  auto generation = base::UnguessableToken::Create();
  int calls = 0;
  ProfileAutoLock timer(base::BindLambdaForTesting([&] { return generation; }),
      base::BindLambdaForTesting([&](ProfileLockReason) { ++calls; generation = {}; }));
  timer.SetIdleTimeout(base::Seconds(1));
  tasks.FastForwardBy(base::Milliseconds(900));
  generation = base::UnguessableToken::Create();
  tasks.FastForwardBy(base::Milliseconds(900)); EXPECT_EQ(0, calls);
  tasks.FastForwardBy(base::Milliseconds(200)); EXPECT_EQ(1, calls);
}
}  // namespace
}  // namespace alloy::site_records
