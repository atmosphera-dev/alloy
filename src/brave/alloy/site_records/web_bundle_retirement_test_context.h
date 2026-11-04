// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_WEB_BUNDLE_RETIREMENT_TEST_CONTEXT_H_
#define BRAVE_ALLOY_SITE_RECORDS_WEB_BUNDLE_RETIREMENT_TEST_CONTEXT_H_
#include <array>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "components/web_package/web_bundle_builder.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/ssl/ssl_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/web_bundle/web_bundle_manager.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
class BundleHandle final : public network::mojom::WebBundleHandle {
 public:
  mojo::PendingRemote<network::mojom::WebBundleHandle> Bind() {
    mojo::PendingRemote<network::mojom::WebBundleHandle> remote;
    receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  void Clone(mojo::PendingReceiver<network::mojom::WebBundleHandle> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }
  void OnWebBundleError(network::mojom::WebBundleErrorType,
                        const std::string&) override {}
  void OnWebBundleLoadFinished(bool) override {}
  void Close() { receivers_.Clear(); }
 private:
  mojo::ReceiverSet<network::mojom::WebBundleHandle> receivers_;
};

class HeldBundleHeaders final : public network::mojom::TrustedHeaderClient {
 public:
  mojo::Remote<network::mojom::TrustedHeaderClient> Bind() {
    mojo::Remote<network::mojom::TrustedHeaderClient> remote;
    receiver_.Bind(remote.BindNewPipeAndPassReceiver());
    return remote;
  }
  void OnBeforeSendHeaders(const GURL&, const net::HttpRequestHeaders&,
                           OnBeforeSendHeadersCallback callback) override {
    std::move(callback).Run(net::OK, std::nullopt, std::nullopt);
  }
  void OnHeadersReceived(const std::string&, const net::IPEndPoint&,
                         const std::optional<net::SSLInfo>&,
                         OnHeadersReceivedCallback callback) override {
    reply = std::move(callback);
    entered.SetValue();
  }
  base::test::TestFuture<void> entered;
  OnHeadersReceivedCallback reply;
 private:
  mojo::Receiver<network::mojom::TrustedHeaderClient> receiver_{this};
};

class WebBundleRetirementTest : public testing::Test {
 public:
  static constexpr int32_t kProcess = 41;
  static GURL BundleURL() { return GURL("https://alpha.test/bundle.wbn"); }
  static GURL ResourceURL() { return GURL("https://alpha.test/data"); }
  static net::IsolationInfo Isolation(const char* owner) {
    return net::IsolationInfo::CreateForInternalRequest(
        url::Origin::Create(GURL(std::string("https://") + owner)));
  }
  base::WeakPtr<network::WebBundleURLLoaderFactory> Create(
      BundleHandle& handle,
      base::UnguessableToken token,
      const char* owner = "alpha.test",
      const GURL& bundle = BundleURL()) {
    network::ResourceRequest::WebBundleTokenParams params(bundle, token,
                                                          handle.Bind());
    return manager().CreateWebBundleURLLoaderFactory(
        bundle, params, kProcess, network::CrossOriginEmbedderPolicy(),
        mojo::NullRemote(), Isolation(owner));
  }
  void Fill(network::WebBundleURLLoaderFactory& factory,
            const std::string& body,
            const char* content_type = "text/plain",
            const GURL& resource = ResourceURL()) {
    web_package::WebBundleBuilder builder;
    builder.AddExchange(resource.spec(),
                        {{":status", "200"}, {"content-type", content_type}},
                        body);
    auto bytes = builder.CreateBundle();
    const MojoCreateDataPipeOptions options = {
        sizeof(options), MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1, 2 * 1024 * 1024};
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    ASSERT_EQ(MOJO_RESULT_OK,
              mojo::CreateDataPipe(&options, producer, consumer));
    factory.SetBundleStream(std::move(consumer));
    size_t written = 0;
    ASSERT_EQ(MOJO_RESULT_OK,
              producer->WriteData(bytes, MOJO_WRITE_DATA_FLAG_NONE, written));
    ASSERT_EQ(bytes.size(), written);
    producer.reset();
  }
  mojo::Remote<network::mojom::URLLoader> Request(
      base::UnguessableToken token,
      network::TestURLLoaderClient& client,
      const char* owner = "alpha.test",
      mojo::Remote<network::mojom::TrustedHeaderClient> headers = {},
      const GURL& bundle = BundleURL(),
      const GURL& resource = ResourceURL()) {
    network::ResourceRequest request;
    request.url = resource;
    request.method = "GET";
    request.request_initiator = Isolation(owner).frame_origin();
    request.web_bundle_token_params =
        network::ResourceRequest::WebBundleTokenParams(bundle, token, kProcess);
    mojo::Remote<network::mojom::URLLoader> loader;
    manager().StartSubresourceRequest(loader.BindNewPipeAndPassReceiver(),
                                     request, client.CreateRemote(), kProcess,
                                     std::move(headers), Isolation(owner));
    return loader;
  }
  base::OnceClosure Prepare(const char* owner, base::OnceClosure done) {
    return manager().PrepareNetworkRetirement(
        base::BindRepeating(
            [](net::NetworkAnonymizationKey original,
               const network::NativeNetworkOwner& candidate) {
              return original == candidate.network_key;
            },
            Isolation(owner).network_anonymization_key()),
        std::move(done));
  }
  std::string Read(mojo::ScopedDataPipeConsumerHandle pipe) {
    std::string output;
    for (;;) {
      std::array<char, 16384> buffer;
      size_t read = 0;
      MojoResult result = pipe->ReadData(
          MOJO_READ_DATA_FLAG_NONE, base::as_writable_byte_span(buffer), read);
      if (result == MOJO_RESULT_FAILED_PRECONDITION)
        return output;
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        base::RunLoop loop;
        mojo::SimpleWatcher watcher(
            FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC);
        watcher.Watch(
            pipe.get(),
            MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
            MOJO_WATCH_CONDITION_SATISFIED,
            base::IgnoreArgs<MojoResult, const mojo::HandleSignalsState&>(
                loop.QuitClosure()));
        loop.Run();
        continue;
      }
      EXPECT_EQ(MOJO_RESULT_OK, result);
      if (result != MOJO_RESULT_OK)
        return output;
      output.append(buffer.data(), read);
    }
  }
 protected:
  network::WebBundleManager& manager() { return *manager_; }
  void UseManager(network::WebBundleManager& manager) { manager_ = &manager; }
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};
 private:
  network::WebBundleManager standalone_manager_;
  network::WebBundleManager* manager_ = &standalone_manager_;
};
}  // namespace alloy::site_records
#endif
