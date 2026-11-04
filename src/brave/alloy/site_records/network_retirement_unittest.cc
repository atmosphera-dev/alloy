// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/memory_coordinator/test_memory_consumer_registry.h"
#include "base/test/simple_test_clock.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "net/base/host_port_pair.h"
#include "net/ssl/ssl_client_session_cache.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace alloy::site_records {
namespace {

class NetworkRetirementCacheTest : public testing::Test {
 protected:
  NetworkRetirementCacheTest()
      : ssl_(SSL_CTX_new(TLS_method())),
        cache_(net::SSLClientSessionCache::Config()) {
    clock_.SetNow(base::Time::FromTimeT(1000000000));
    cache_.SetClockForTesting(&clock_);
  }

  static net::SSLClientSessionCache::Key Key(const char* host, int port = 443) {
    net::SSLClientSessionCache::Key key;
    key.server = net::HostPortPair(host, port);
    return key;
  }

  bssl::UniquePtr<SSL_SESSION> Session() {
    bssl::UniquePtr<SSL_SESSION> session(SSL_SESSION_new(ssl_.get()));
    if (!session || !SSL_SESSION_set_protocol_version(session.get(), TLS1_2_VERSION))
      return nullptr;
    SSL_SESSION_set_time(session.get(), clock_.Now().ToTimeT());
    SSL_SESSION_set_timeout(session.get(), 3600);
    return session;
  }

  auto Admission(const char* host, int port = 443) {
    return cache_.CaptureInsertion(Key(host, port));
  }

  void RetireAlpha() {
    cache_.FlushMatching(base::BindRepeating(
        [](const net::SSLClientSessionCache::Key& key) {
          return key.server.host() == "alpha.test";
        }));
  }

  base::test::TaskEnvironment tasks_;
  base::TestMemoryConsumerRegistry registry_;
  base::SimpleTestClock clock_;
  bssl::UniquePtr<SSL_CTX> ssl_;
  net::SSLClientSessionCache cache_;
};

TEST_F(NetworkRetirementCacheTest, CachedSessionsRetireSelectively) {
  auto alpha = Session();
  auto alternate = Session();
  auto beta = Session();
  ASSERT_TRUE(alpha && alternate && beta);
  cache_.Insert(*Admission("alpha.test"), Key("alpha.test"), bssl::UpRef(alpha));
  cache_.Insert(*Admission("alpha.test", 9443), Key("alpha.test", 9443),
                bssl::UpRef(alternate));
  cache_.Insert(*Admission("beta.test"), Key("beta.test"), bssl::UpRef(beta));
  ASSERT_EQ(alpha.get(), cache_.Lookup(Key("alpha.test")).get());
  ASSERT_EQ(alternate.get(), cache_.Lookup(Key("alpha.test", 9443)).get());
  ASSERT_EQ(beta.get(), cache_.Lookup(Key("beta.test")).get());

  RetireAlpha();

  EXPECT_EQ(nullptr, cache_.Lookup(Key("alpha.test")).get());
  EXPECT_EQ(nullptr, cache_.Lookup(Key("alpha.test", 9443)).get());
  EXPECT_EQ(beta.get(), cache_.Lookup(Key("beta.test")).get());
}

TEST_F(NetworkRetirementCacheTest, UncachedOldHandshakeCannotPublish) {
  auto alpha = Session();
  ASSERT_TRUE(alpha);
  const auto original = Admission("alpha.test");
  ASSERT_EQ(nullptr, cache_.Lookup(Key("alpha.test")).get());

  RetireAlpha();
  cache_.Insert(*original, Key("alpha.test"), bssl::UpRef(alpha));

  EXPECT_EQ(nullptr, cache_.Lookup(Key("alpha.test")).get());
}

TEST_F(NetworkRetirementCacheTest, UnrelatedHeldHandshakeStillPublishes) {
  auto beta = Session();
  ASSERT_TRUE(beta);
  const auto original = Admission("beta.test");
  ASSERT_EQ(nullptr, cache_.Lookup(Key("beta.test")).get());

  RetireAlpha();
  cache_.Insert(*original, Key("beta.test"), bssl::UpRef(beta));

  EXPECT_EQ(beta.get(), cache_.Lookup(Key("beta.test")).get());
}

TEST_F(NetworkRetirementCacheTest, FreshHandshakeCanPublishAfterRetirement) {
  auto alpha = Session();
  ASSERT_TRUE(alpha);
  RetireAlpha();

  cache_.Insert(*Admission("alpha.test"), Key("alpha.test"), bssl::UpRef(alpha));

  EXPECT_EQ(alpha.get(), cache_.Lookup(Key("alpha.test")).get());
}

TEST_F(NetworkRetirementCacheTest, GlobalConfigurationStillRejectsOldAdmission) {
  auto beta = Session();
  ASSERT_TRUE(beta);
  const auto original = Admission("beta.test");
  cache_.Flush();

  cache_.Insert(*original, Key("beta.test"), bssl::UpRef(beta));
  EXPECT_EQ(nullptr, cache_.Lookup(Key("beta.test")).get());
  cache_.Insert(*Admission("beta.test"), Key("beta.test"), bssl::UpRef(beta));
  EXPECT_EQ(beta.get(), cache_.Lookup(Key("beta.test")).get());
}

TEST_F(NetworkRetirementCacheTest, AdmissionCannotMoveBetweenCacheOwnersOrKeys) {
  auto alpha = Session();
  ASSERT_TRUE(alpha);
  const auto original = Admission("alpha.test");
  net::SSLClientSessionCache other{net::SSLClientSessionCache::Config()};
  other.SetClockForTesting(&clock_);

  other.Insert(*original, Key("alpha.test"), bssl::UpRef(alpha));
  EXPECT_EQ(nullptr, other.Lookup(Key("alpha.test")).get());
  cache_.Insert(*original, Key("beta.test"), bssl::UpRef(alpha));
  EXPECT_EQ(nullptr, cache_.Lookup(Key("beta.test")).get());
  cache_.Insert(*original, Key("alpha.test"), bssl::UpRef(alpha));
  EXPECT_EQ(alpha.get(), cache_.Lookup(Key("alpha.test")).get());
}

}  // namespace
}  // namespace alloy::site_records
