// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/accessibility_client.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "cef/include/cef_api_hash.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::guest::accessibility {
namespace {
class Handler final : public AlloyGuestAccessibilityHandler {
 public:
  void OnUpdate(CefRefPtr<CefDictionaryValue>) override {
    std::lock_guard lock(mutex_); ++updates_;
  }
  void OnUnavailable() override {
    std::lock_guard lock(mutex_); ++unavailable_; cv_.notify_all();
  }
  void OnClosed() override {}
  bool WaitUnavailable() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return unavailable_ != 0; });
  }
  int unavailable() { std::lock_guard lock(mutex_); return unavailable_; }
  int updates() { std::lock_guard lock(mutex_); return updates_; }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int unavailable_ = 0, updates_ = 0;
  IMPLEMENT_REFCOUNTING(Handler);
};

class GuestAccessibilityClient : public testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    ASSERT_NE(cef_api_hash(CEF_API_VERSION, 0), nullptr);
    ExtensionEndpoint endpoint;
    endpoint.attach = [this](Feature feature, std::shared_ptr<ExtensionSink> sink) {
      EXPECT_EQ(feature, Feature::kAccessibility);
      sink_ = std::move(sink); sink_->OwnerChanged(owner_); return true;
    };
    endpoint.call = [](Feature, std::string operation, std::string, ExtensionCompletion) {
      EXPECT_EQ(operation, "start"); return true;
    };
    endpoint.owned_call = [this](Feature feature, Owner owner, std::string operation,
                                std::string, ExtensionCompletion completion) {
      EXPECT_EQ(feature, Feature::kAccessibility); EXPECT_EQ(owner, owner_);
      EXPECT_EQ(operation, "resync"); ++requests_; complete_ = std::move(completion); return true;
    };
    client_ = MakeClient(std::move(endpoint)); handler_ = new Handler;
    ASSERT_TRUE(client_->Start(handler_));
  }
  void TearDown() override {
    if (sink_) { sink_->Retire(); sink_->Wait(); }
  }
  void Part(const std::string& sequence, const std::string& total,
            const std::string& offset, const std::string& content) {
    std::string packed;
    ASSERT_TRUE(PackFields({sequence, total, offset, content}, &packed));
    sink_->Event(owner_, "update-part", std::move(packed));
  }
  const Owner owner_{1, 1, 1, 1};
  CefRefPtr<AlloyGuestAccessibility> client_;
  CefRefPtr<Handler> handler_;
  std::shared_ptr<ExtensionSink> sink_;
  ExtensionCompletion complete_;
  int requests_ = 0;
};

TEST_P(GuestAccessibilityClient, RejectedResyncResponseNotifiesActualHandlerAfterReply) {
  // Enter the real adapter's automatic recovery path, then inject its response
  // through the same ordered extension sink used by the session client.
  sink_->Event(owner_, "update-part", "malformed initial update");
  ASSERT_EQ(requests_, 1); ASSERT_TRUE(complete_);
  if (GetParam() == "MalformedMultipart") {
    Part("2", "4", "1", "bad");
  } else if (GetParam() == "IncompleteMultipart") {
    Part("2", "4", "0", "{");
  } else if (GetParam() == "MalformedJson") {
    Part("2", "4", "0", "oops");
  } else {
    const std::string json = R"({"sequence":"3"})";
    Part("2", std::to_string(json.size()), "0", json);
  }
  EXPECT_EQ(requests_, 1); EXPECT_EQ(handler_->unavailable(), 0);
  auto complete = std::move(complete_);
  complete(ProviderResult::kOk, 42, {});
  EXPECT_TRUE(handler_->WaitUnavailable());
  EXPECT_EQ(handler_->unavailable(), 1); EXPECT_EQ(handler_->updates(), 0);
  EXPECT_EQ(requests_, 1);
}

INSTANTIATE_TEST_SUITE_P(TransportValidation, GuestAccessibilityClient,
    testing::Values("MalformedMultipart", "IncompleteMultipart", "MalformedJson", "SequenceMismatch"),
    [](const testing::TestParamInfo<std::string>& info) { return info.param; });
}  // namespace
}  // namespace alloy::guest::accessibility
