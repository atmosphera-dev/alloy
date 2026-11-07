// Copyright (c) 2023 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CONTENT_BROWSER_FILTER_LIST_SERVICE_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CONTENT_BROWSER_FILTER_LIST_SERVICE_H_
#include <string>
#include "base/functional/callback.h"
#include "brave/components/brave_shields/core/common/filter_list.mojom.h"
#include "components/keyed_service/core/keyed_service.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
namespace brave_shields {
class AdBlockService;
class FilterListService:public KeyedService,public mojom::FilterListAndroidHandler {
 public:
  explicit FilterListService(AdBlockService*);
  explicit FilterListService(base::RepeatingCallback<AdBlockService*()> original);
  ~FilterListService() override;
  mojo::PendingRemote<mojom::FilterListAndroidHandler> MakeRemote();
  void Bind(mojo::PendingReceiver<mojom::FilterListAndroidHandler>);
  void IsFilterListEnabled(const std::string&,IsFilterListEnabledCallback) override;
  void EnableFilter(const std::string&,bool) override;
  void GetSubscriptions(GetSubscriptionsCallback) override;
  void CreateSubscription(const GURL&) override;
  void EnableSubscription(const GURL&,bool) override;
  void DeleteSubscription(const GURL&) override;
  void GetFilterLists(GetFilterListsCallback) override;
  void GetCustomFilters(GetCustomFiltersCallback) override;
  void UpdateCustomFilters(const std::string&,UpdateCustomFiltersCallback) override;
  void UpdateFilterLists(UpdateFilterListsCallback) override;
 private:
  AdBlockService* Service();
  base::RepeatingCallback<AdBlockService*()> original_;
  mojo::ReceiverSet<mojom::FilterListAndroidHandler> receivers_;
  FilterListService(const FilterListService&)=delete;
  FilterListService& operator=(const FilterListService&)=delete;
};
}  // namespace brave_shields
#endif
