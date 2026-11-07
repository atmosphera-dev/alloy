/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "brave/browser/ui/webui/brave_adblock_ui.h"
#include <memory>
#include <string>
#include "base/check_op.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "brave/alloy/site_records/profile_subscription_source.h"
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/browser/ui/webui/brave_webui_source.h"
#include "brave/components/brave_adblock/resources/grit/brave_adblock_generated_map.h"
#include "brave/components/brave_shields/content/browser/ad_block_custom_filters_provider.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/content/browser/ad_block_subscription_service_manager.h"
#include "brave/components/brave_shields/content/browser/ad_block_subscription_service_manager_observer.h"
#include "brave/components/brave_shields/core/browser/ad_block_component_service_manager.h"
#include "brave/components/constants/webui_url_constants.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "components/grit/brave_components_resources.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui_message_handler.h"
#if BUILDFLAG(IS_ANDROID)
#include "chrome/browser/android/tab_web_contents_delegate_android.h"
#else
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/singleton_tabs.h"
#endif
namespace {
class AdblockDOMHandler:public content::WebUIMessageHandler,
    public brave_shields::AdBlockSubscriptionServiceManagerObserver {
 public:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback("brave_adblock.enableFilterList",base::BindRepeating(&AdblockDOMHandler::Enable,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.getCustomFilters",base::BindRepeating(&AdblockDOMHandler::GetCustom,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.getRegionalLists",base::BindRepeating(&AdblockDOMHandler::GetRegional,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.getListSubscriptions",base::BindRepeating(&AdblockDOMHandler::GetSubscriptions,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.updateCustomFilters",base::BindRepeating(&AdblockDOMHandler::UpdateCustom,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.submitNewSubscription",base::BindRepeating(&AdblockDOMHandler::Submit,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.setSubscriptionEnabled",base::BindRepeating(&AdblockDOMHandler::SetEnabled,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.deleteSubscription",base::BindRepeating(&AdblockDOMHandler::Delete,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.refreshSubscription",base::BindRepeating(&AdblockDOMHandler::Refresh,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock.viewSubscriptionSource",base::BindRepeating(&AdblockDOMHandler::ViewSource,base::Unretained(this)));
  }
  void OnJavascriptAllowed() override {if(auto* service=Service())observer_.Observe(service->subscription_service_manager());}
  void OnJavascriptDisallowed() override {observer_.Reset();}
  void OnServiceUpdateEvent() override {if(IsJavascriptAllowed())PublishSubscriptions();}
 private:
  brave_shields::AdBlockService* Service(){return brave_shields::AdBlockServiceForContext(*Profile::FromWebUI(web_ui()));}
  void Enable(const base::ListValue& args) {
    DCHECK_EQ(args.size(),2u);if(!args[0].is_string()||!args[1].is_bool())return;
    if(auto* service=Service())service->component_service_manager()->EnableFilterList(args[0].GetString(),args[1].GetBool());
  }
  void GetCustom(const base::ListValue& args) {
    DCHECK_EQ(args.size(),0u);AllowJavascript();if(auto* service=Service())
      CallJavascriptFunction("brave_adblock.onGetCustomFilters",base::Value(service->custom_filters_provider()->GetCustomFilters()));
  }
  void GetRegional(const base::ListValue& args) {
    DCHECK_EQ(args.size(),0u);AllowJavascript();if(auto* service=Service())
      CallJavascriptFunction("brave_adblock.onGetRegionalLists",service->component_service_manager()->GetRegionalLists());
  }
  void GetSubscriptions(const base::ListValue& args){DCHECK_EQ(args.size(),0u);AllowJavascript();PublishSubscriptions();}
  void UpdateCustom(const base::ListValue& args) {
    DCHECK_EQ(args.size(),1u);if(!args[0].is_string())return;
    if(auto* service=Service())service->custom_filters_provider()->UpdateCustomFiltersFromSettings(Profile::FromWebUI(web_ui())->GetPrefs(),args[0].GetString());
  }
  void Submit(const base::ListValue& args) {
    DCHECK_EQ(args.size(),1u);AllowJavascript();if(!args[0].is_string())return;
    const GURL url(args[0].GetString());if(!url.is_valid())return;
    if(auto* service=Service()){service->subscription_service_manager()->CreateSubscription(url);PublishSubscriptions();}
  }
  void SetEnabled(const base::ListValue& args) {
    DCHECK_EQ(args.size(),2u);AllowJavascript();if(!args[0].is_string()||!args[1].is_bool())return;
    const GURL url(args[0].GetString());if(!url.is_valid())return;
    if(auto* service=Service()){service->subscription_service_manager()->EnableSubscription(url,args[1].GetBool());PublishSubscriptions();}
  }
  void Delete(const base::ListValue& args) {
    DCHECK_EQ(args.size(),1u);AllowJavascript();if(!args[0].is_string())return;
    const GURL url(args[0].GetString());if(!url.is_valid())return;
    if(auto* service=Service()){service->subscription_service_manager()->DeleteSubscription(url);PublishSubscriptions();}
  }
  void Refresh(const base::ListValue& args) {
    DCHECK_EQ(args.size(),1u);AllowJavascript();if(!args[0].is_string())return;
    const GURL url(args[0].GetString());if(!url.is_valid())return;
    if(auto* service=Service())service->subscription_service_manager()->RefreshSubscription(url,true);
  }
  void ViewSource(const base::ListValue& args) {
    DCHECK_EQ(args.size(),1u);if(!args[0].is_string())return;
    const GURL url(args[0].GetString());if(!url.is_valid())return;auto* service=Service();if(!service)return;
    const GURL source=service->subscription_service_manager()->GetListTextFileUrl(url);if(!source.is_valid())return;
#if BUILDFLAG(IS_ANDROID)
    web_ui()->GetWebContents()->GetDelegate()->OpenURLFromTab(web_ui()->GetWebContents(),
        content::OpenURLParams(source,content::Referrer(),WindowOpenDisposition::NEW_FOREGROUND_TAB,ui::PAGE_TRANSITION_AUTO_TOPLEVEL,false),{});
#else
    auto* browser=GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(web_ui()->GetWebContents());
    ShowSingletonTabOverwritingNTP(browser,source);
#endif
  }
  void PublishSubscriptions() {
    auto* service=Service();if(!service)return;base::ListValue list;
    for(const auto& subscription:service->subscription_service_manager()->GetSubscriptions()) {
      base::DictValue value;value.Set("subscription_url",subscription.subscription_url.spec());value.Set("enabled",subscription.enabled);
      value.Set("last_update_attempt",subscription.last_update_attempt.InMillisecondsFSinceUnixEpoch());
      value.Set("last_successful_update_attempt",subscription.last_successful_update_attempt.InMillisecondsFSinceUnixEpoch());
      if(subscription.homepage)value.Set("homepage",*subscription.homepage);
      if(subscription.title)value.Set("title",*subscription.title);list.Append(std::move(value));
    }CallJavascriptFunction("brave_adblock.onGetListSubscriptions",list);
  }
  base::ScopedObservation<brave_shields::AdBlockSubscriptionServiceManager,
      brave_shields::AdBlockSubscriptionServiceManagerObserver> observer_{this};
};
}  // namespace
BraveAdblockUI::BraveAdblockUI(content::WebUI* ui):WebUIController(ui) {
  // Resolve the original input before installing its source route, including
  // transient contexts whose first consumer is this page.
  brave_shields::AdBlockServiceForContext(*Profile::FromWebUI(ui));
  auto* source=CreateAndAddWebUIDataSource(ui,kAdblockHost,kBraveAdblockGenerated,IDR_BRAVE_ADBLOCK_HTML);
  alloy::site_records::InstallProfileSubscriptionSource(*source,
      alloy::site_records::ProfileFilterPreferences::Get(*Profile::FromWebUI(ui)));
  ui->AddMessageHandler(std::make_unique<AdblockDOMHandler>());
}
BraveAdblockUI::~BraveAdblockUI()=default;
