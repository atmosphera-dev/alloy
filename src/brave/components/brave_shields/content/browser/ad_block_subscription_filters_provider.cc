// Copyright (c) 2021 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/content/browser/ad_block_subscription_filters_provider.h"

#include <string>
#include <utility>
#include "base/check.h"

#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider.h"
#include "brave/components/brave_shields/core/common/adblock/rs/src/lib.rs.h"
#include "components/prefs/pref_service.h"

namespace brave_shields {

namespace {

// static
void AddDATBufferToFilterSet(
    base::OnceCallback<void(const adblock::FilterListMetadata&)> on_metadata,
    DATFileDataBuffer buffer,
    std::shared_ptr<std::atomic<bool>> alive,
    base::RepeatingCallback<bool()> admitted,
    const perfetto::Flow& flow,
    rust::Box<adblock::FilterSet>* filter_set) {
  TRACE_EVENT("brave.adblock",
              "AddDATBufferToFilterSet_SubscriptionFiltersProvider", flow);
  if (!alive->load(std::memory_order_acquire) || (admitted && !admitted.Run())) return;
  auto result = (*filter_set)->add_filter_list(buffer);
  if (result.result_kind == adblock::ResultKind::Success) {
    std::move(on_metadata).Run(result.value.metadata);
  }
}

}  // namespace

AdBlockSubscriptionFiltersProvider::AdBlockSubscriptionFiltersProvider(
    AdBlockFiltersProviderManager* manager,
    base::FilePath list_file,
    base::RepeatingCallback<void(const adblock::FilterListMetadata&)>
        on_metadata_retrieved)
    : AdBlockFiltersProvider(false, manager),
      list_file_(list_file),
      on_metadata_retrieved_(on_metadata_retrieved) {}

AdBlockSubscriptionFiltersProvider::AdBlockSubscriptionFiltersProvider(
    AdBlockFiltersProviderManager* manager, Source source,
    base::RepeatingCallback<void(const adblock::FilterListMetadata&)> metadata)
    : AdBlockFiltersProvider(false, manager), source_(std::move(source)), on_metadata_retrieved_(std::move(metadata)) {
  CHECK(source_->read && source_->admitted);
}
AdBlockSubscriptionFiltersProvider::~AdBlockSubscriptionFiltersProvider() {
  alive_->store(false, std::memory_order_release);
}

void AdBlockSubscriptionFiltersProvider::LoadFilterSet(
    base::OnceCallback<
        void(base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>)> cb) {
  const auto flow = perfetto::Flow::FromPointer(this);
  TRACE_EVENT("brave.adblock",
               "AdBlockSubscriptionFiltersProvider::LoadFilterSet", flow);
  if (source_) {
    source_->read.Run(base::BindOnce(
        [](base::WeakPtr<AdBlockSubscriptionFiltersProvider> self,
           base::OnceCallback<void(base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>)> cb,
           const perfetto::Flow& flow, SourceResult status, DATFileDataBuffer bytes) {
          if (!self) { std::move(cb).Run(base::BindOnce([](rust::Box<adblock::FilterSet>*) {})); return; }
          if (status == SourceResult::kError) return;
          if (status == SourceResult::kCancelled) {
            std::move(cb).Run(base::BindOnce([](rust::Box<adblock::FilterSet>*) {})); return;
          }
          self->OnDATFileDataReady(std::move(cb), flow, bytes);
        }, weak_factory_.GetWeakPtr(), std::move(cb), flow));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&brave_component_updater::ReadDATFileData, list_file_),
      base::BindOnce(&AdBlockSubscriptionFiltersProvider::OnDATFileDataReady,
                     weak_factory_.GetWeakPtr(), std::move(cb), flow));
}

std::string AdBlockSubscriptionFiltersProvider::GetNameForDebugging() {
  return "AdBlockSubscriptionFiltersProvider";
}

void AdBlockSubscriptionFiltersProvider::OnDATFileDataReady(
    base::OnceCallback<
        void(base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>)> cb,
    const perfetto::Flow& flow,
    const DATFileDataBuffer& dat_buf) {
  TRACE_EVENT("brave.adblock",
              "AdBlockSubscriptionFiltersProvider::OnDATFileDataReady", flow);
  std::move(cb).Run(base::BindOnce(
      &AddDATBufferToFilterSet,
      base::BindOnce(
          [](scoped_refptr<base::SequencedTaskRunner> task_runner,
             std::shared_ptr<std::atomic<bool>> alive,
             base::RepeatingCallback<bool()> admitted,
             base::RepeatingCallback<void(const adblock::FilterListMetadata&)>
                 on_metadata,
             const adblock::FilterListMetadata& metadata) {
            task_runner->PostTask(FROM_HERE, base::BindOnce(
                [](std::shared_ptr<std::atomic<bool>> alive, base::RepeatingCallback<bool()> admitted,
                   base::RepeatingCallback<void(const adblock::FilterListMetadata&)> callback,
                   const adblock::FilterListMetadata& metadata) {
                  if (alive->load(std::memory_order_acquire) && (!admitted || admitted.Run())) callback.Run(metadata);
                }, alive, admitted, on_metadata, metadata));
          },
           base::SingleThreadTaskRunner::GetCurrentDefault(),
           alive_, source_ ? source_->admitted : base::RepeatingCallback<bool()>(),
           on_metadata_retrieved_),
      dat_buf, alive_, source_ ? source_->admitted : base::RepeatingCallback<bool()>(), flow));
}

void AdBlockSubscriptionFiltersProvider::OnListAvailable() {
  NotifyObservers(engine_is_default_);
}

}  // namespace brave_shields
