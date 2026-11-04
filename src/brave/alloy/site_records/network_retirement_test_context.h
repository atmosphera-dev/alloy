// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_NETWORK_RETIREMENT_TEST_CONTEXT_H_
#define BRAVE_ALLOY_SITE_RECORDS_NETWORK_RETIREMENT_TEST_CONTEXT_H_
#include "base/check.h"
#include "base/files/file.h"
#include "base/files/scoped_temp_dir.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/dns/mock_host_resolver.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_utils.h"

namespace alloy::site_records {
class NetworkRetirementTestContext {
 public:
  explicit NetworkRetirementTestContext(
      base::OnceCallback<void(net::URLRequestContextBuilder*)> configure = {}) {
    service_ = network::NetworkService::CreateForTesting();
    auto resolver = std::make_unique<net::MockHostResolver>();
    resolver_ = resolver.get();
    resolver_->rules()->AddRule("*", "127.0.0.1");
    service_->set_host_resolver_factory_for_testing(
        std::make_unique<ResolverFactory>(std::move(resolver)));
    auto params = network::CreateNetworkContextParamsForTesting();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    CHECK(log_dir_.CreateUniqueTempDir());
    params->alloy_guard_log =
        base::File(log_dir_.GetPath().AppendASCII("guard.log"),
                   base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    context_ = network::NetworkContext::CreateForTesting(
        service_.get(), remote_.BindNewPipeAndPassReceiver(), std::move(params),
        std::move(configure));
  }
  network::NetworkContext& context() { return *context_; }
  net::MockHostResolver& resolver() { return *resolver_; }
  mojo::Remote<network::mojom::NetworkContext>& remote() { return remote_; }

 private:
  class ResolverFactory final : public net::HostResolver::Factory {
   public:
    explicit ResolverFactory(std::unique_ptr<net::HostResolver> resolver)
        : resolver_(std::move(resolver)) {}
    std::unique_ptr<net::HostResolver> CreateResolver(net::HostResolverManager*,
                                                      std::string_view,
                                                      bool,
                                                      bool) override {
      CHECK(resolver_);
      return std::move(resolver_);
    }
    std::unique_ptr<net::HostResolver> CreateStandaloneResolver(
        net::NetLog*,
        const net::HostResolver::ManagerOptions&,
        std::string_view,
        bool,
        bool) override {
      CHECK(resolver_);
      return std::move(resolver_);
    }

   private:
    std::unique_ptr<net::HostResolver> resolver_;
  };
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  base::ScopedTempDir log_dir_;
#endif
  std::unique_ptr<network::NetworkService> service_;
  mojo::Remote<network::mojom::NetworkContext> remote_;
  std::unique_ptr<network::NetworkContext> context_;
  raw_ptr<net::MockHostResolver> resolver_ = nullptr;
};
}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_NETWORK_RETIREMENT_TEST_CONTEXT_H_
