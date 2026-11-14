// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/visited_link_policy.h"

#include <memory>
#include "base/files/file_util.h"
#include "components/visitedlink/browser/partitioned_visitedlink_writer.h"
#include "components/visitedlink/browser/visitedlink_delegate.h"
#include "components/visitedlink/browser/visitedlink_writer.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "net/base/schemeful_site.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace alloy::site_records {
namespace {
class BrowserOwner : public base::SupportsUserData {
 public:
  ~BrowserOwner() override = default;
};
class Delegate final : public visitedlink::VisitedLinkDelegate {
 public:
  void RebuildTable(const scoped_refptr<URLEnumerator>& value) override { urls = value; }
  void BuildVisitedLinkTable(const scoped_refptr<VisitedLinkEnumerator>& value) override { links = value; }
  void Finish() {
    if (urls) { urls->OnComplete(true); urls = nullptr; }
    if (links) { links->OnVisitedLinkComplete(true); links = nullptr; }
  }
  void Existing(const GURL& url) {
    if (urls) urls->OnURL(url);
    if (links) links->OnVisitedLink(url, net::SchemefulSite(url), url::Origin::Create(url));
  }
  scoped_refptr<URLEnumerator> urls;
  scoped_refptr<VisitedLinkEnumerator> links;
};
class Writers : public testing::TestWithParam<bool> {
 protected:
  bool Init(bool finish = true) {
    if (GetParam()) {
      partitioned_ = std::make_unique<visitedlink::PartitionedVisitedLinkWriter>(&context_, &delegate_);
    } else {
      // Request persistence deliberately. Configured native projections must
      // refuse it independently of this caller preference and OTR's value.
      legacy_ = std::make_unique<visitedlink::VisitedLinkWriter>(&context_, &delegate_, true);
    }
    const bool initialized = GetParam() ? partitioned_->Init() : legacy_->Init();
    if (finish) Finish();
    return initialized;
  }
  void Finish() { delegate_.Finish(); environment_.RunUntilIdle(); }
  void Add(const GURL& url) {
    if (GetParam()) partitioned_->AddVisitedLink({url, net::SchemefulSite(url), url::Origin::Create(url)});
    else legacy_->AddURL(url);
    environment_.RunUntilIdle();
  }
  int Count() { return GetParam() ? partitioned_->GetUsedCount() : legacy_->GetUsedCount(); }
  bool Contains(const GURL& url) {
    if (!GetParam()) return legacy_->IsVisited(url);
    auto salt = partitioned_->GetOrAddOriginSalt(url::Origin::Create(url));
    return salt && partitioned_->IsVisited(url, net::SchemefulSite(url), url::Origin::Create(url), *salt);
  }
  void NoFile() {
    environment_.RunUntilIdle();
    EXPECT_FALSE(base::PathExists(context_.GetPath().AppendASCII("Visited Links")));
  }
  content::BrowserTaskEnvironment environment_;
  content::TestBrowserContext context_;
  Delegate delegate_;
  std::unique_ptr<visitedlink::VisitedLinkWriter> legacy_;
  std::unique_ptr<visitedlink::PartitionedVisitedLinkWriter> partitioned_;
};
TEST_P(Writers, MemoryContextAdmitsResidentLinksWithoutFiles) {
  context_.set_is_off_the_record(true);
  RequestContextPolicy::AttachMemory(context_);
  ASSERT_TRUE(Init());
  const GURL first("https://example.test/first");
  Add(first);
  EXPECT_TRUE(Contains(first));
  EXPECT_GT(Count(), 0);
  NoFile();
}
TEST_P(Writers, StatelessRefusesInitializationAndLateAdds) {
  context_.set_is_off_the_record(true);
  RequestContextPolicy::AttachStateless(context_, NetworkState::kKeep);
  EXPECT_FALSE(Init());
  Add(GURL("https://example.test/refused"));
  EXPECT_EQ(0, Count()); NoFile();
}
TEST_P(Writers, OrdinaryOffTheRecordStillRefuses) {
  context_.set_is_off_the_record(true);
  EXPECT_FALSE(Init());
  Add(GURL("https://example.test/refused"));
  EXPECT_EQ(0, Count()); NoFile();
}
TEST_P(Writers, LockedOriginalProfileCannotInitializeAProjection) {
  RecordCapabilityIssuer profile(base::UnguessableToken::Create(), base::UnguessableToken::Create(),
                                 base::UnguessableToken::Create(), StorageMode::kEncrypted);
  RequestContextPolicy::AttachEncrypted(context_, profile.Issue(StorePurpose::kProfileSettings));
  profile.Revoke();
  EXPECT_FALSE(Init());
  Add(GURL("https://example.test/refused"));
  EXPECT_EQ(0, Count()); NoFile();
}
TEST_P(Writers, OriginalGenerationRevocationRefusesFurtherAdmission) {
  RecordCapabilityIssuer profile(base::UnguessableToken::Create(), base::UnguessableToken::Create(),
                                 base::UnguessableToken::Create(), StorageMode::kEncrypted);
  RequestContextPolicy::AttachEncrypted(context_, profile.Issue(StorePurpose::kProfileSettings));
  ASSERT_TRUE(Init());
  const GURL first("https://example.test/before-lock");
  Add(first); EXPECT_TRUE(Contains(first));
  const int before = Count();
  profile.Revoke();
  Add(GURL("https://example.test/after-lock"));
  EXPECT_EQ(before, Count());
  if (GetParam()) EXPECT_FALSE(partitioned_->GetOrAddOriginSalt(url::Origin::Create(first)));
  NoFile();
}
TEST_P(Writers, DelayedRebuildCannotPublishAfterRevocation) {
  context_.set_is_off_the_record(true);
  RequestContextPolicy::AttachMemory(context_);
  ASSERT_TRUE(Init(false));
  delegate_.Existing(GURL("https://example.test/delayed"));
  RequestContextPolicy::Get(context_)->Revoke();
  Finish();
  EXPECT_EQ(0, Count()); NoFile();
}
INSTANTIATE_TEST_SUITE_P(BothNativeWriters, Writers, testing::Bool());

TEST(VisitedLinkPolicy, PersistenceComesFromContextAuthorityNotCallerPreference) {
  BrowserOwner regular, memory, stateless, encrypted;
  EXPECT_TRUE(VisitedLinkPolicy(&regular, false).CanPersist(true));
  EXPECT_FALSE(VisitedLinkPolicy(&regular, true).CanPersist(true));
  RequestContextPolicy::AttachMemory(memory);
  RequestContextPolicy::AttachStateless(stateless, NetworkState::kCut);
  RecordCapabilityIssuer profile(base::UnguessableToken::Create(), base::UnguessableToken::Create(),
                                 base::UnguessableToken::Create(), StorageMode::kEncrypted);
  RequestContextPolicy::AttachEncrypted(encrypted, profile.Issue(StorePurpose::kProfileSettings));
  for (const auto* context : {&memory, &stateless, &encrypted}) {
    VisitedLinkPolicy policy(context, false);
    EXPECT_FALSE(policy.CanPersist(true));
  }
  VisitedLinkPolicy original(&encrypted, false);
  EXPECT_TRUE(original.CanWriteMemory());
  profile.Revoke();
  EXPECT_FALSE(original.CanWriteMemory());
}
}  // namespace
}  // namespace alloy::site_records
