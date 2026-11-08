// Copyright (c) 2022 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"
#include <limits>
#include "base/check.h"
#include "base/check_op.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/notreached.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/components/brave_shields/core/common/features.h"
namespace brave_shields {
AdBlockFiltersProviderManager::AdBlockFiltersProviderManager()
    : AdBlockFiltersProviderManager(base::FeatureList::IsEnabled(features::kAdblockDATCache)) {}
AdBlockFiltersProviderManager::AdBlockFiltersProviderManager(bool use_dat_cache) {
  suppress_default_engine_startup_change_notification_=use_dat_cache;
  suppress_additional_engine_startup_change_notification_=suppress_default_engine_startup_change_notification_;
}
AdBlockFiltersProviderManager::~AdBlockFiltersProviderManager()=default;
void AdBlockFiltersProviderManager::InvalidateLoads(bool which) {
  for(auto it=pending_.begin();it!=pending_.end();) {
    if(it->second.is_default==which)it=pending_.erase(it);else ++it;
  }
  NotifyInvalidated(which);
}
void AdBlockFiltersProviderManager::AddProvider(AdBlockFiltersProvider* provider,bool which) {
  auto rv=which ? default_engine_filters_providers_.insert(provider) : additional_engine_filters_providers_.insert(provider);
  DCHECK(rv.second);provider->AddObserver(this);InvalidateLoads(which);
}
void AdBlockFiltersProviderManager::RemoveProvider(AdBlockFiltersProvider* provider,bool which) {
  auto& providers=which ? default_engine_filters_providers_ : additional_engine_filters_providers_;
  auto found=providers.find(provider);DCHECK(found!=providers.end());providers.erase(found);
  InvalidateLoads(which);NotifyObservers(which);
}
void AdBlockFiltersProviderManager::MaybeNotifyObserver(AdBlockFiltersProvider::Observer& observer,bool which) {
  if(AreAllProvidersInitialized(which))observer.OnChanged(which);
}
void AdBlockFiltersProviderManager::ForceNotifyObserver(AdBlockFiltersProvider::Observer& observer,bool which) {
  if(MaybeConsumeEngineStartupChangeNotification(which))return;
  if(AreAllProvidersInitialized(which))observer.OnChanged(which);
}
std::string AdBlockFiltersProviderManager::GetNameForDebugging(){return "AdBlockFiltersProviderManager";}
const base::flat_set<AdBlockFiltersProvider*>& AdBlockFiltersProviderManager::GetProviders(bool which) const {
  return which ? default_engine_filters_providers_ : additional_engine_filters_providers_;
}
void AdBlockFiltersProviderManager::OnChanged(bool which) {
  InvalidateLoads(which);
  if(!AreAllProvidersInitialized(which))return;
  if(MaybeConsumeEngineStartupChangeNotification(which))return;
  NotifyObservers(which);
}
bool AdBlockFiltersProviderManager::MaybeConsumeEngineStartupChangeNotification(bool which) {
  auto& suppressed=which ? suppress_default_engine_startup_change_notification_ : suppress_additional_engine_startup_change_notification_;
  if(suppressed){suppressed=false;return true;}return false;
}
bool AdBlockFiltersProviderManager::AreAllProvidersInitialized(bool which) const {
  const auto& providers=GetProviders(which);if(providers.empty())return false;
  for(auto* provider:providers)if(!provider->IsInitialized())return false;
  return true;
}
void AdBlockFiltersProviderManager::LoadFilterSet(Loaded){NOTREACHED();}
void AdBlockFiltersProviderManager::LoadFilterSetForEngine(bool which,Loaded done) {
  CHECK_NE(next_load_,std::numeric_limits<uint64_t>::max());const auto id=++next_load_;
  const auto& providers=GetProviders(which);
  pending_.emplace(id,Pending{which,std::move(done),{},providers.size()});
  if(providers.empty()){FinishLoad(id);return;}
  for(auto* provider:providers)task_tracker_.PostTask(
      base::SequencedTaskRunner::GetCurrentDefault().get(),FROM_HERE,
      base::BindOnce(&AdBlockFiltersProvider::LoadFilterSet,provider->AsWeakPtr(),
          base::BindOnce(&AdBlockFiltersProviderManager::SourceLoaded,weak_factory_.GetWeakPtr(),id)));
}
void AdBlockFiltersProviderManager::SourceLoaded(uint64_t id,Parse parse) {
  auto found=pending_.find(id);if(found==pending_.end())return;
  auto& load=found->second;load.results.push_back(std::move(parse));
  CHECK_GT(load.remaining,0u);if(--load.remaining==0)FinishLoad(id);
}
void AdBlockFiltersProviderManager::FinishLoad(uint64_t id) {
  auto found=pending_.find(id);CHECK(found!=pending_.end());
  auto done=std::move(found->second.done);auto results=std::move(found->second.results);pending_.erase(found);
  // Preserve the native completion order while keeping private callbacks owned
  // here until the entire snapshot is ready. External loaders hold only id+weak.
  std::move(done).Run(base::BindOnce([](std::vector<Parse> results,rust::Box<adblock::FilterSet>* set) {
    for(auto& parse:results)std::move(parse).Run(set);
  },std::move(results)));
}
}  // namespace brave_shields
