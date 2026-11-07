// Copyright (c) 2023 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#include "brave/components/brave_shields/content/browser/filter_list_service.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/content/browser/ad_block_subscription_service_manager.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_service_manager.h"
#include "ui/base/l10n/time_format.h"
namespace brave_shields {
FilterListService::FilterListService(AdBlockService* service):FilterListService(base::BindRepeating(
    [](base::WeakPtr<AdBlockService> service)->AdBlockService* {return service&&!service->IsClosed()?service.get():nullptr;},service?service->GetWeakPtr():base::WeakPtr<AdBlockService>())) {}
FilterListService::FilterListService(base::RepeatingCallback<AdBlockService*()> original):original_(std::move(original)){CHECK(original_);}
FilterListService::~FilterListService()=default;
AdBlockService* FilterListService::Service(){auto* service=original_.Run();if(!service)receivers_.Clear();return service;}
mojo::PendingRemote<mojom::FilterListAndroidHandler> FilterListService::MakeRemote(){mojo::PendingRemote<mojom::FilterListAndroidHandler> remote;receivers_.Add(this,remote.InitWithNewPipeAndPassReceiver());return remote;}
void FilterListService::Bind(mojo::PendingReceiver<mojom::FilterListAndroidHandler> receiver){receivers_.Add(this,std::move(receiver));}
void FilterListService::IsFilterListEnabled(const std::string& id,IsFilterListEnabledCallback done){if(auto* service=Service())std::move(done).Run(service->component_service_manager()->IsFilterListEnabled(id));}
void FilterListService::EnableFilter(const std::string& id,bool enabled){if(auto* service=Service())service->component_service_manager()->EnableFilterList(id,enabled);}
void FilterListService::GetFilterLists(GetFilterListsCallback done){if(auto* service=Service())std::move(done).Run(service->component_service_manager()->GetRegionalLists());}
void FilterListService::GetSubscriptions(GetSubscriptionsCallback done) {
  auto* service=Service();if(!service)return;std::vector<mojom::SubscriptionInfoPtr> items;
  const auto now=base::Time::Now();for(const auto& subscription:service->subscription_service_manager()->GetSubscriptions()) {
    auto info=mojom::SubscriptionInfo::New();info->enabled=subscription.enabled;
    if(subscription.title)info->title=*subscription.title;
    auto elapsed=now-subscription.last_successful_update_attempt;if(elapsed<base::TimeDelta())elapsed=base::TimeDelta();
    if(subscription.homepage)info->homepage=*subscription.homepage;
    info->subscription_url=subscription.subscription_url;info->last_update_attempt=subscription.last_update_attempt;
    info->last_successful_update_attempt=subscription.last_successful_update_attempt;
    info->last_updated_pretty_text=base::UTF16ToUTF8(ui::TimeFormat::Simple(ui::TimeFormat::Format::FORMAT_ELAPSED,ui::TimeFormat::Length::LENGTH_LONG,elapsed));
    info->expires=subscription.expires;items.push_back(std::move(info));
  }std::move(done).Run(std::move(items));
}
void FilterListService::CreateSubscription(const GURL& url){if(auto* service=Service())service->subscription_service_manager()->CreateSubscription(url);}
void FilterListService::EnableSubscription(const GURL& url,bool enabled){if(auto* service=Service())service->subscription_service_manager()->EnableSubscription(url,enabled);}
void FilterListService::DeleteSubscription(const GURL& url){if(auto* service=Service())service->subscription_service_manager()->DeleteSubscription(url);}
void FilterListService::GetCustomFilters(GetCustomFiltersCallback done){if(auto* service=Service())std::move(done).Run(service->custom_filters_provider()->GetCustomFilters());}
void FilterListService::UpdateCustomFilters(const std::string& text,UpdateCustomFiltersCallback done){if(auto* service=Service())std::move(done).Run(service->custom_filters_provider()->UpdateCustomFilters(text));}
void FilterListService::UpdateFilterLists(UpdateFilterListsCallback done){if(auto* service=Service())service->component_service_manager()->UpdateFilterLists(std::move(done));}
}  // namespace brave_shields
