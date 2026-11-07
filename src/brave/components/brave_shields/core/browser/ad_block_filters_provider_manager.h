// Copyright (c) 2022 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_FILTERS_PROVIDER_MANAGER_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_FILTERS_PROVIDER_MANAGER_H_
#include <map>
#include <string>
#include <vector>
#include "base/containers/flat_set.h"
#include "base/functional/callback.h"
#include "base/task/cancelable_task_tracker.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider.h"
namespace brave_shields {
class AdBlockFiltersProviderManager : public AdBlockFiltersProvider,
                                      public AdBlockFiltersProvider::Observer {
 public:
  using Parse=base::OnceCallback<void(rust::Box<adblock::FilterSet>*)>;
  using Loaded=base::OnceCallback<void(Parse)>;
  AdBlockFiltersProviderManager();
  ~AdBlockFiltersProviderManager() override;
  AdBlockFiltersProviderManager(const AdBlockFiltersProviderManager&)=delete;
  AdBlockFiltersProviderManager& operator=(const AdBlockFiltersProviderManager&)=delete;
  void LoadFilterSet(Loaded) override;
  void LoadFilterSetForEngine(bool is_default,Loaded);
  void OnChanged(bool is_default) override;
  void AddProvider(AdBlockFiltersProvider*,bool is_default);
  void RemoveProvider(AdBlockFiltersProvider*,bool is_default);
  void MaybeNotifyObserver(AdBlockFiltersProvider::Observer&,bool is_default);
  void ForceNotifyObserver(AdBlockFiltersProvider::Observer&,bool is_default);
  std::string GetNameForDebugging() override;
  const base::flat_set<AdBlockFiltersProvider*>& GetProviders(bool is_default) const;
 private:
  bool MaybeConsumeEngineStartupChangeNotification(bool);
  bool AreAllProvidersInitialized(bool) const;
  void InvalidateLoads(bool);
  void SourceLoaded(uint64_t,Parse);
  void FinishLoad(uint64_t);
  struct Pending {
    bool is_default;Loaded done;std::vector<Parse> results;size_t remaining;
  };
  std::map<uint64_t,Pending> pending_;
  uint64_t next_load_=0;
  base::flat_set<AdBlockFiltersProvider*> default_engine_filters_providers_;
  base::flat_set<AdBlockFiltersProvider*> additional_engine_filters_providers_;
  bool suppress_default_engine_startup_change_notification_=false;
  bool suppress_additional_engine_startup_change_notification_=false;
  base::CancelableTaskTracker task_tracker_;
  base::WeakPtrFactory<AdBlockFiltersProviderManager> weak_factory_{this};
};
}  // namespace brave_shields
#endif
