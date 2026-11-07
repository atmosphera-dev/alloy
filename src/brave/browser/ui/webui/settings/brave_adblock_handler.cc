/* Copyright (c) 2022 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "brave/browser/ui/webui/settings/brave_adblock_handler.h"
#include <string>
#include <utility>
#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/values.h"
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_service_manager.h"
#include "brave/components/brave_shields/core/common/pref_names.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/time_format.h"
namespace {
brave_shields::AdBlockService* Service(content::WebUI* ui) {
  return brave_shields::AdBlockServiceForContext(*Profile::FromWebUI(ui));
}
}  // namespace
BraveAdBlockHandler::BraveAdBlockHandler()=default;
BraveAdBlockHandler::~BraveAdBlockHandler()=default;
void BraveAdBlockHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback("brave_adblock.getRegionalLists",base::BindRepeating(&BraveAdBlockHandler::GetRegionalLists,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.enableFilterList",base::BindRepeating(&BraveAdBlockHandler::EnableFilterList,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.updateFilterLists",base::BindRepeating(&BraveAdBlockHandler::UpdateFilterLists,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.getListSubscriptions",base::BindRepeating(&BraveAdBlockHandler::GetListSubscriptions,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.getCustomFilters",base::BindRepeating(&BraveAdBlockHandler::GetCustomFilters,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.addSubscription",base::BindRepeating(&BraveAdBlockHandler::AddSubscription,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.setSubscriptionEnabled",base::BindRepeating(&BraveAdBlockHandler::SetSubscriptionEnabled,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.updateSubscription",base::BindRepeating(&BraveAdBlockHandler::UpdateSubscription,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.deleteSubscription",base::BindRepeating(&BraveAdBlockHandler::DeleteSubscription,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.viewSubscription",base::BindRepeating(&BraveAdBlockHandler::ViewSubscriptionSource,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.updateCustomFilters",base::BindRepeating(&BraveAdBlockHandler::UpdateCustomFilters,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.getCustomScriptlets",base::BindRepeating(&BraveAdBlockHandler::GetCustomScriptlets,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.addCustomScriptlet",base::BindRepeating(&BraveAdBlockHandler::AddCustomScriptlet,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.updateCustomScriptlet",base::BindRepeating(&BraveAdBlockHandler::UpdateCustomScriptlet,base::Unretained(this)));
  web_ui()->RegisterMessageCallback("brave_adblock.removeCustomScriptlet",base::BindRepeating(&BraveAdBlockHandler::RemoveCustomScriptlet,base::Unretained(this)));
  if(auto* prefs=brave_shields::AdBlockPrefsForContext(*Profile::FromWebUI(web_ui())))pref_change_registrar_.Init(prefs);
}
void BraveAdBlockHandler::OnJavascriptAllowed() {
  auto* service=Service(web_ui());if(!service)return;
  service_observer_.Observe(service->subscription_service_manager());
  custom_resources_observer_.Observe(service->custom_resource_provider());
  pref_change_registrar_.Add(brave_shields::prefs::kAdBlockCustomFilters,
      base::BindRepeating(&BraveAdBlockHandler::RefreshCustomFilters,weak_factory_.GetWeakPtr()));
}
void BraveAdBlockHandler::OnJavascriptDisallowed() {
  weak_factory_.InvalidateWeakPtrs();service_observer_.Reset();custom_resources_observer_.Reset();pref_change_registrar_.RemoveAll();
}
void BraveAdBlockHandler::OnServiceUpdateEvent(){if(IsJavascriptAllowed())RefreshSubscriptionsList();}
void BraveAdBlockHandler::OnCustomResourcesChanged(){if(IsJavascriptAllowed())FireWebUIListener("brave_adblock.onCustomResourcesChanged");}
void BraveAdBlockHandler::GetRegionalLists(const base::ListValue& args) {
  AllowJavascript();auto* service=Service(web_ui());
  if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  ResolveJavascriptCallback(args[0],service->component_service_manager()->GetRegionalLists());
}
void BraveAdBlockHandler::EnableFilterList(const base::ListValue& args) {
  DCHECK_EQ(args.size(),2u);if(!args[0].is_string()||!args[1].is_bool())return;
  if(auto* service=Service(web_ui()))service->component_service_manager()->EnableFilterList(args[0].GetString(),args[1].GetBool());
}
void BraveAdBlockHandler::UpdateFilterLists(const base::ListValue& args) {
  AllowJavascript();DCHECK_EQ(args.size(),1u);if(!args[0].is_string())return;
  auto* service=Service(web_ui());if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  service->component_service_manager()->UpdateFilterLists(base::BindOnce(&BraveAdBlockHandler::OnFilterListsUpdated,weak_factory_.GetWeakPtr(),args[0].GetString()));
}
void BraveAdBlockHandler::GetListSubscriptions(const base::ListValue& args) {
  AllowJavascript();if(!Service(web_ui())){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  ResolveJavascriptCallback(args[0],GetSubscriptions());
}
void BraveAdBlockHandler::GetCustomFilters(const base::ListValue& args) {
  AllowJavascript();auto* service=Service(web_ui());
  if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  ResolveJavascriptCallback(args[0],base::Value(service->custom_filters_provider()->GetCustomFilters()));
}
void BraveAdBlockHandler::AddSubscription(const base::ListValue& args) {
  DCHECK_EQ(args.size(),1u);AllowJavascript();if(!args[0].is_string())return;
  const GURL url(args[0].GetString());if(!url.is_valid())return;
  if(auto* service=Service(web_ui())){service->subscription_service_manager()->CreateSubscription(url);RefreshSubscriptionsList();}
}
void BraveAdBlockHandler::SetSubscriptionEnabled(const base::ListValue& args) {
  DCHECK_EQ(args.size(),2u);AllowJavascript();if(!args[0].is_string()||!args[1].is_bool())return;
  const GURL url(args[0].GetString());if(!url.is_valid())return;
  if(auto* service=Service(web_ui())){service->subscription_service_manager()->EnableSubscription(url,args[1].GetBool());RefreshSubscriptionsList();}
}
void BraveAdBlockHandler::UpdateSubscription(const base::ListValue& args) {
  DCHECK_EQ(args.size(),1u);AllowJavascript();if(!args[0].is_string())return;
  const GURL url(args[0].GetString());if(!url.is_valid())return;
  if(auto* service=Service(web_ui()))service->subscription_service_manager()->RefreshSubscription(url,true);
}
void BraveAdBlockHandler::DeleteSubscription(const base::ListValue& args) {
  DCHECK_EQ(args.size(),1u);AllowJavascript();if(!args[0].is_string())return;
  const GURL url(args[0].GetString());if(!url.is_valid())return;
  if(auto* service=Service(web_ui())){service->subscription_service_manager()->DeleteSubscription(url);RefreshSubscriptionsList();}
}
void BraveAdBlockHandler::ViewSubscriptionSource(const base::ListValue& args) {
  DCHECK_EQ(args.size(),1u);if(!args[0].is_string())return;
  const GURL url(args[0].GetString());if(!url.is_valid())return;auto* service=Service(web_ui());if(!service)return;
  auto source=service->subscription_service_manager()->GetListTextFileUrl(url);if(!source.is_valid())return;
  auto* browser=GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(web_ui()->GetWebContents());
  ShowSingletonTabOverwritingNTP(browser,source);
}
void BraveAdBlockHandler::UpdateCustomFilters(const base::ListValue& args) {
  if(!args[0].is_string())return;
  if(auto* service=Service(web_ui()))service->custom_filters_provider()->UpdateCustomFilters(args[0].GetString());
}
void BraveAdBlockHandler::GetCustomScriptlets(const base::ListValue& args) {
  CHECK(args.size()==1u&&args[0].is_string());AllowJavascript();auto* service=Service(web_ui());
  if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  service->custom_resource_provider()->GetCustomResourcesWithStatus(base::BindOnce(
      [](base::WeakPtr<BraveAdBlockHandler> self,std::string id,
          brave_shields::AdBlockCustomResourceProvider::ErrorCode status,base::Value value) {
        if(!self||!self->IsJavascriptAllowed())return;
        if(status==brave_shields::AdBlockCustomResourceProvider::ErrorCode::kOk)self->OnGetCustomScriptlets(id,std::move(value));
        else self->RejectJavascriptCallback(base::Value(id),base::Value(static_cast<int>(status)));
      },weak_factory_.GetWeakPtr(),args[0].GetString()));
}
void BraveAdBlockHandler::OnGetCustomScriptlets(const std::string& id,base::Value value) {
  if(IsJavascriptAllowed())ResolveJavascriptCallback(base::Value(id),value);
}
void BraveAdBlockHandler::AddCustomScriptlet(const base::ListValue& args) {
  CHECK(args.size()==2u&&args[0].is_string()&&args[1].is_dict());AllowJavascript();auto* service=Service(web_ui());
  if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  service->custom_resource_provider()->AddResource(Profile::FromWebUI(web_ui())->GetPrefs(),args[1],
      base::BindOnce(&BraveAdBlockHandler::OnScriptletUpdateStatus,weak_factory_.GetWeakPtr(),args[0].GetString()));
}
void BraveAdBlockHandler::UpdateCustomScriptlet(const base::ListValue& args) {
  CHECK(args.size()==3u&&args[0].is_string()&&args[1].is_string()&&args[2].is_dict());AllowJavascript();auto* service=Service(web_ui());
  if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  service->custom_resource_provider()->UpdateResource(Profile::FromWebUI(web_ui())->GetPrefs(),args[1].GetString(),args[2],
      base::BindOnce(&BraveAdBlockHandler::OnScriptletUpdateStatus,weak_factory_.GetWeakPtr(),args[0].GetString()));
}
void BraveAdBlockHandler::RemoveCustomScriptlet(const base::ListValue& args) {
  CHECK(args.size()==2u&&args[0].is_string()&&args[1].is_string());AllowJavascript();auto* service=Service(web_ui());
  if(!service){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
  service->custom_resource_provider()->RemoveResource(Profile::FromWebUI(web_ui())->GetPrefs(),args[1].GetString(),
      base::BindOnce(&BraveAdBlockHandler::OnScriptletUpdateStatus,weak_factory_.GetWeakPtr(),args[0].GetString()));
}
void BraveAdBlockHandler::OnScriptletUpdateStatus(const std::string& id,brave_shields::AdBlockCustomResourceProvider::ErrorCode code) {
  if(IsJavascriptAllowed())ResolveJavascriptCallback(base::Value(id),base::Value(static_cast<int>(code)));
}
void BraveAdBlockHandler::RefreshSubscriptionsList(){if(Service(web_ui()))FireWebUIListener("brave_adblock.onGetListSubscriptions",GetSubscriptions());}
void BraveAdBlockHandler::RefreshCustomFilters() {
  if(auto* service=Service(web_ui()))FireWebUIListener("brave_adblock.onCustomFiltersChanged",base::Value(service->custom_filters_provider()->GetCustomFilters()));
}
base::ListValue BraveAdBlockHandler::GetSubscriptions() {
  base::ListValue list;auto* service=Service(web_ui());if(!service)return list;
  const auto now=base::Time::Now();for(const auto& subscription:service->subscription_service_manager()->GetSubscriptions()) {
    base::DictValue value;value.Set("subscription_url",subscription.subscription_url.spec());value.Set("enabled",subscription.enabled);
    value.Set("last_update_attempt",subscription.last_update_attempt.InMillisecondsFSinceUnixEpoch());
    value.Set("last_successful_update_attempt",subscription.last_successful_update_attempt.InMillisecondsFSinceUnixEpoch());
    value.Set("last_updated_pretty_text",ui::TimeFormat::Simple(ui::TimeFormat::Format::FORMAT_ELAPSED,ui::TimeFormat::Length::LENGTH_LONG,now-subscription.last_successful_update_attempt));
    if(subscription.homepage)value.Set("homepage",*subscription.homepage);
    value.Set("title",subscription.title&&!subscription.title->empty()?*subscription.title:subscription.subscription_url.spec());
    list.Append(std::move(value));
  }return list;
}
void BraveAdBlockHandler::OnFilterListsUpdated(std::string id,bool success) {
  if(!IsJavascriptAllowed())return;
  if(success)ResolveJavascriptCallback(base::Value(id),base::Value());else RejectJavascriptCallback(base::Value(id),base::Value());
}
