// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/request_owner.h"

#include <memory>
#include <utility>
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::guest::integration {
namespace {
constexpr Owner kOriginal{1, 2, 3, 4};
constexpr Owner kReplacement{1, 3, 4, 5};

TEST(GuestIntegrationRequests, NativeAdmissionPrecedesAppRequest) {
  RequestOwner requests;
  int completions = 0;
  EXPECT_EQ(requests.Begin(kOriginal, [] { return false; },
      [&](bool allowed, std::string details, std::vector<std::string> bytes) {
        ++completions;
        EXPECT_FALSE(allowed);
        EXPECT_TRUE(details.empty());
        EXPECT_TRUE(bytes.empty());
      }, {"private"}), 0u);
  EXPECT_EQ(completions, 1);
  EXPECT_EQ(requests.pending(), 0u);
}

TEST(GuestIntegrationRequests, ExactOwnerAndCheckedOutgoingRanges) {
  RequestOwner requests;
  auto id = requests.Begin(kOriginal, [] { return true; },
      [](bool, std::string, std::vector<std::string>) {}, {std::string("\0abc", 4)});
  ASSERT_NE(id, 0u);
  EXPECT_FALSE(requests.Read(kReplacement, id, 0, 0, 1));
  EXPECT_FALSE(requests.Read(kOriginal, id, 1, 0, 1));
  EXPECT_FALSE(requests.Read(kOriginal, id, 0, UINT64_MAX, 1));
  EXPECT_FALSE(requests.Read(kOriginal, id, 0, 1, UINT64_MAX));
  EXPECT_EQ(requests.Read(kOriginal, id, 0, 1, 3), "abc");
  EXPECT_EQ(requests.Read(kOriginal, id, 0, 4, 0), "");
  EXPECT_EQ(requests.pending(), 1u);
}

TEST(GuestIntegrationRequests, PartialDuplicateAndOverlappingDataCannotComplete) {
  RequestOwner requests;
  int completed = 0;
  auto id = requests.Begin(kOriginal, [] { return true; },
      [&](bool allowed, std::string details, std::vector<std::string> bytes) {
        ++completed;
        EXPECT_TRUE(allowed);
        EXPECT_EQ(details, "metadata");
        ASSERT_EQ(bytes.size(), 2u);
        EXPECT_EQ(bytes[0], "abcd");
        EXPECT_TRUE(bytes[1].empty());
      }, {});
  EXPECT_FALSE(requests.Finish(kOriginal, id, true, {}));
  EXPECT_TRUE(requests.Respond(kOriginal, id, {4, 0}));
  EXPECT_FALSE(requests.Respond(kOriginal, id, {1}));
  EXPECT_FALSE(requests.Append(kReplacement, id, 0, 0, "ab"));
  EXPECT_TRUE(requests.Append(kOriginal, id, 0, 0, "ab"));
  EXPECT_FALSE(requests.Append(kOriginal, id, 0, 0, "ab"));
  EXPECT_FALSE(requests.Append(kOriginal, id, 0, 2, "cde"));
  EXPECT_FALSE(requests.Finish(kOriginal, id, true, {}));
  EXPECT_EQ(completed, 0);
  EXPECT_TRUE(requests.Append(kOriginal, id, 0, 2, "cd"));
  EXPECT_TRUE(requests.Finish(kOriginal, id, true, "metadata"));
  EXPECT_FALSE(requests.Finish(kOriginal, id, true, "metadata"));
  EXPECT_EQ(completed, 1);
}

TEST(GuestIntegrationRequests, RetirementRevokesReadAndHeldPartialReply) {
  RequestOwner requests;
  bool admitted = true;
  int completed = 0;
  auto id = requests.Begin(kOriginal, [&] { return admitted; },
      [&](bool allowed, std::string, std::vector<std::string> bytes) {
        ++completed;
        EXPECT_FALSE(allowed);
        EXPECT_TRUE(bytes.empty());
      }, {"original"});
  EXPECT_TRUE(requests.Respond(kOriginal, id, {10}));
  EXPECT_TRUE(requests.Append(kOriginal, id, 0, 0, "partial"));
  admitted = false;
  EXPECT_FALSE(requests.Read(kOriginal, id, 0, 0, 8));
  EXPECT_FALSE(requests.Append(kOriginal, id, 0, 7, "end"));
  EXPECT_FALSE(requests.Finish(kOriginal, id, true, "stale"));
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(requests.pending(), 0u);
}

TEST(GuestIntegrationRequests, CancelDoesNotCancelReplacement) {
  RequestOwner requests;
  int original = 0, replacement = 0;
  requests.Begin(kOriginal, [] { return true; },
      [&](bool allowed, std::string, std::vector<std::string>) {
        EXPECT_FALSE(allowed); ++original;
      }, {});
  auto id = requests.Begin(kReplacement, [] { return true; },
      [&](bool allowed, std::string, std::vector<std::string>) {
        EXPECT_FALSE(allowed); ++replacement;
      }, {});
  requests.Cancel(kOriginal);
  EXPECT_EQ(original, 1);
  EXPECT_EQ(replacement, 0);
  EXPECT_EQ(requests.pending(), 1u);
  EXPECT_TRUE(requests.Finish(kReplacement, id, false, {}));
  EXPECT_EQ(replacement, 1);
}

TEST(GuestIntegrationRequests, CompletionRetiresBeforeReentry) {
  RequestOwner requests;
  uint64_t id = 0;
  int completed = 0;
  id = requests.Begin(kOriginal, [] { return true; },
      [&](bool allowed, std::string, std::vector<std::string>) {
        ++completed;
        EXPECT_FALSE(allowed);
        EXPECT_EQ(requests.pending(), 0u);
        EXPECT_FALSE(requests.Finish(kOriginal, id, false, {}));
      }, {});
  EXPECT_TRUE(requests.Finish(kOriginal, id, false, {}));
  EXPECT_EQ(completed, 1);
}

TEST(GuestIntegrationRequests, CancellationMayDestroyRegistry) {
  auto requests = std::make_unique<RequestOwner>();
  int completed = 0;
  for (int i = 0; i < 3; ++i)
    requests->Begin(kOriginal, [] { return true; },
        [&](bool allowed, std::string, std::vector<std::string>) {
          EXPECT_FALSE(allowed);
          ++completed;
          requests.reset();
        }, {});
  requests->Cancel(kOriginal);
  EXPECT_FALSE(requests);
  EXPECT_EQ(completed, 3);
}

TEST(GuestIntegrationRequests, ProviderLossRetiresAndRefusesNewRequests) {
  RequestOwner requests;
  int completed = 0;
  auto complete = [&](bool allowed, std::string, std::vector<std::string>) {
    EXPECT_FALSE(allowed); ++completed;
  };
  auto id = requests.Begin(kOriginal, [] { return true; }, complete, {});
  requests.Close();
  EXPECT_EQ(completed, 1);
  EXPECT_FALSE(requests.Finish(kOriginal, id, false, {}));
  EXPECT_EQ(requests.Begin(kReplacement, [] { return true; }, complete, {}), 0u);
  requests.Close();
  EXPECT_EQ(completed, 2);
}

}  // namespace
}  // namespace alloy::guest::integration
