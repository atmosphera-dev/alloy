// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_subscription_fetch.h"

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/sequence_checker.h"
#include "brave/components/brave_shields/content/browser/ad_block_subscription_download_manager.h"
#include "net/base/load_flags.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"

namespace alloy::site_records {
namespace {
using Provider = brave_shields::AdBlockSubscriptionFiltersProvider;
using Result = Provider::SourceResult;

class Fetch final : public base::RefCounted<Fetch>,
                    public network::SimpleURLLoaderStreamConsumer {
 public:
  Fetch(base::RepeatingCallback<bool()> admitted, Provider::SourceReply reply)
      : admitted_(std::move(admitted)), reply_(std::move(reply)) {}

  void Start(network::SharedURLLoaderFactory* factory, const GURL& url,
             bool from_ui) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!admitted_.Run()) {
      Finish(Result::kCancelled);
      return;
    }
    if (!url.SchemeIsHTTPOrHTTPS()) {
      Finish(Result::kError);
      return;
    }
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->load_flags = net::LOAD_DISABLE_CACHE;
    request->priority = from_ui ? net::HIGHEST : net::LOW;
    loader_ = network::SimpleURLLoader::Create(std::move(request),
        brave_shields::AdBlockSubscriptionDownloadManager::TrafficAnnotation());
    loader_->SetOnRedirectCallback(base::BindRepeating(
        &Fetch::Redirect, base::Unretained(this)));
    // Stream rather than impose DownloadToString's unrelated 5 MiB ceiling.
    // Native list parsing already requires the complete byte vector. Chunks
    // avoid reallocating private payload buffers while receiving the response.
    loader_->DownloadAsStream(factory, this);
  }

  void Cancel() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    Finish(Result::kCancelled);
  }

 private:
  friend class base::RefCounted<Fetch>;
  ~Fetch() override { Wipe(); }

  void Wipe() {
    for (auto& chunk : chunks_)
      Cleanse(chunk);
    chunks_.clear();
    size_ = 0;
  }

  void Finish(Result status) {
    scoped_refptr<Fetch> alive(this);
    if (!reply_)
      return;
    loader_.reset();
    if (status == Result::kOk && !admitted_.Run())
      status = Result::kCancelled;
    DATFileDataBuffer result;
    if (status == Result::kOk) {
      result.reserve(size_);
      for (const auto& chunk : chunks_)
        result.insert(result.end(), chunk.begin(), chunk.end());
    }
    Wipe();
    std::move(reply_).Run(status, std::move(result));
  }

  void Redirect(const GURL&, const net::RedirectInfo& redirect,
                const network::mojom::URLResponseHead&,
                std::vector<std::string>*) {
    if (!admitted_.Run())
      Finish(Result::kCancelled);
    else if (!redirect.new_url.SchemeIsHTTPOrHTTPS())
      Finish(Result::kError);
  }

  void OnDataReceived(std::string_view bytes, base::OnceClosure resume) override {
    scoped_refptr<Fetch> alive(this);
    if (!admitted_.Run()) {
      Finish(Result::kCancelled);
      return;
    }
    if (bytes.size() > DATFileDataBuffer().max_size() - size_) {
      Finish(Result::kError);
      return;
    }
    chunks_.emplace_back(bytes.begin(), bytes.end());
    size_ += bytes.size();
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    Finish(!admitted_.Run() ? Result::kCancelled :
           success ? Result::kOk : Result::kError);
  }

  void OnRetry(base::OnceClosure retry) override {
    scoped_refptr<Fetch> alive(this);
    Wipe();
    if (!admitted_.Run())
      Finish(Result::kCancelled);
    else
      std::move(retry).Run();
  }

  const base::RepeatingCallback<bool()> admitted_;
  Provider::SourceReply reply_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::vector<DATFileDataBuffer> chunks_;
  size_t size_ = 0;
  SEQUENCE_CHECKER(sequence_checker_);
};
}  // namespace

ProfileFilterPreferences::SubscriptionFetch MakeProfileSubscriptionFetch(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    base::RepeatingCallback<bool()> admitted) {
  CHECK(factory && admitted);
  return base::BindRepeating(
      [](scoped_refptr<network::SharedURLLoaderFactory> factory,
         base::RepeatingCallback<bool()> admitted, const GURL& url, bool from_ui,
         Provider::SourceReply reply) -> base::OnceClosure {
        auto request = base::MakeRefCounted<Fetch>(admitted, std::move(reply));
        request->Start(factory.get(), url, from_ui);
        return base::BindOnce(&Fetch::Cancel, request);
      }, std::move(factory), std::move(admitted));
}
}  // namespace alloy::site_records
