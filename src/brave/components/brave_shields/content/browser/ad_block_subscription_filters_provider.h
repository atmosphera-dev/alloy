// Copyright (c) 2021 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CONTENT_BROWSER_AD_BLOCK_SUBSCRIPTION_FILTERS_PROVIDER_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CONTENT_BROWSER_AD_BLOCK_SUBSCRIPTION_FILTERS_PROVIDER_H_
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/trace_event/trace_event.h"
#include "brave/components/brave_component_updater/browser/dat_file_util.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"
#include "brave/components/brave_shields/core/common/adblock/rs/src/lib.rs.h"
#include "third_party/rust/cxx/v1/cxx.h"
using brave_component_updater::DATFileDataBuffer;
namespace brave_shields {
class AdBlockSubscriptionFiltersProvider : public AdBlockFiltersProvider {
 public:
  enum class SourceResult { kOk, kMissing, kCancelled, kError };
  using SourceReply = base::OnceCallback<void(SourceResult, DATFileDataBuffer)>;
  struct Source {
    base::RepeatingCallback<void(SourceReply)> read;
    // Callable on engine/I/O sequences as well as UI; original generation only.
    base::RepeatingCallback<bool()> admitted;
  };
  AdBlockSubscriptionFiltersProvider(AdBlockFiltersProviderManager*, base::FilePath,
      base::RepeatingCallback<void(const adblock::FilterListMetadata&)>);
  AdBlockSubscriptionFiltersProvider(AdBlockFiltersProviderManager*, Source,
      base::RepeatingCallback<void(const adblock::FilterListMetadata&)>);
  ~AdBlockSubscriptionFiltersProvider() override;
  void LoadFilterSet(base::OnceCallback<void(base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>)>) override;
  void OnListAvailable();
 private:
  void OnDATFileDataReady(base::OnceCallback<void(base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>)>,
      const perfetto::Flow&, const DATFileDataBuffer&);
  std::string GetNameForDebugging() override;
  base::FilePath list_file_;
  std::optional<Source> source_;
  const std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  base::RepeatingCallback<void(const adblock::FilterListMetadata&)> on_metadata_retrieved_;
  base::WeakPtrFactory<AdBlockSubscriptionFiltersProvider> weak_factory_{this};
};
}  // namespace brave_shields
#endif
