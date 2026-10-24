// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <string>
#include <vector>

#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/test/mock_navigation_handle.h"
#include "content/public/test/mock_navigation_throttle_registry.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {
using Registry = content::MockNavigationThrottleRegistry;

class RecordingThrottle : public content::NavigationThrottle {
 public:
  RecordingThrottle(content::NavigationThrottleRegistry& registry,
                    std::string name, std::vector<std::string>& calls)
      : NavigationThrottle(registry), name_(std::move(name)), calls_(calls) {}
  const char* GetNameForLogging() override { return name_.c_str(); }
  ThrottleCheckResult WillProcessResponse() override {
    calls_.push_back(name_);
    return PROCEED;
  }
 private:
  const std::string name_;
  std::vector<std::string>& calls_;
};

class NavigationMockPriorityTest : public content::RenderViewHostTestHarness {
 protected:
  void CheckPriority(Registry::RegistrationMode mode) {
    auto navigation = content::NavigationSimulator::CreateBrowserInitiated(
        GURL("https://navigation-mock.test/page"), web_contents());
    navigation->Start();
    std::vector<std::string> calls;
    Registry registry(navigation->GetNavigationHandle(), mode);
    navigation->GetNavigationHandle()->RegisterThrottleForTesting(
        std::make_unique<RecordingThrottle>(registry, "already-registered", calls));
    registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "normal-a", calls));
    registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "first-a", calls), true);
    registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "first-b", calls), true);
    registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "normal-b", calls));
    if (mode == Registry::RegistrationMode::kHold) {
      ASSERT_EQ(registry.throttles().size(), 4u);
      EXPECT_STREQ(registry.throttles().front()->GetNameForLogging(), "first-b");
      registry.RegisterHeldThrottles();
      EXPECT_TRUE(registry.throttles().empty());
    }
    navigation->ReadyToCommit();
    EXPECT_EQ(calls, (std::vector<std::string>{"first-b", "first-a",
        "already-registered", "normal-a", "normal-b"}));
    navigation->Commit();
  }
};

TEST_F(NavigationMockPriorityTest, AutoRegistrationRunsFirstBeforeOtherThrottles) {
  CheckPriority(Registry::RegistrationMode::kAutoRegistrationForTesting);
}

TEST_F(NavigationMockPriorityTest, HeldRegistrationRunsFirstBeforeOtherThrottles) {
  CheckPriority(Registry::RegistrationMode::kHold);
}

TEST_F(NavigationMockPriorityTest, ClearingHeldEntriesClearsPriorityForNextAdd) {
  auto navigation = content::NavigationSimulator::CreateBrowserInitiated(
      GURL("https://navigation-mock.test/clear"), web_contents());
  navigation->Start();
  std::vector<std::string> calls;
  Registry registry(navigation->GetNavigationHandle(), Registry::RegistrationMode::kHold);
  navigation->GetNavigationHandle()->RegisterThrottleForTesting(
      std::make_unique<RecordingThrottle>(registry, "existing", calls));
  registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "removed", calls), true);
  registry.throttles().clear();
  registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "ordinary", calls));
  registry.RegisterHeldThrottles();
  navigation->ReadyToCommit();
  EXPECT_EQ(calls, (std::vector<std::string>{"existing", "ordinary"}));
  navigation->Commit();
}

TEST_F(NavigationMockPriorityTest, LegacyMockCallAndPriorityCallRemainDistinct) {
  content::MockNavigationHandle handle;
  Registry registry(&handle);
  std::vector<std::string> calls;
  EXPECT_CALL(handle, RegisterThrottleForTesting(testing::_)).Times(1);
  EXPECT_CALL(handle, RegisterFirstThrottleForTesting(testing::_)).Times(1);
  registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "ordinary", calls));
  registry.AddThrottle(std::make_unique<RecordingThrottle>(registry, "priority", calls), true);
}
}  // namespace
