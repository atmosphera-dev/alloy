// Copyright (c) 2023 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#include "brave/browser/ui/webui/brave_adblock_internals_ui.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "base/byte_size.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/browser/ui/webui/brave_webui_source.h"
#include "brave/components/brave_adblock/adblock_internals/resources/grit/brave_adblock_internals_generated_map.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "components/grit/brave_components_resources.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/resource_coordinator/public/cpp/memory_instrumentation/memory_instrumentation.h"
namespace {
class BraveAdblockInternalsMessageHandler:public content::WebUIMessageHandler {
 private:
  brave_shields::AdBlockService* Service(){return brave_shields::AdBlockServiceForContext(*web_ui()->GetWebContents()->GetBrowserContext());}
  void OnJavascriptDisallowed() override {weak_.InvalidateWeakPtrs();}
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback("brave_adblock_internals.getDebugInfo",base::BindRepeating(&BraveAdblockInternalsMessageHandler::GetDebugInfo,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock_internals.discardRegex",base::BindRepeating(&BraveAdblockInternalsMessageHandler::DiscardRegex,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock_internals.setDebugMode",base::BindRepeating(&BraveAdblockInternalsMessageHandler::SetDebugMode,base::Unretained(this)));
    web_ui()->RegisterMessageCallback("brave_adblock_internals.dropDATCache",base::BindRepeating(&BraveAdblockInternalsMessageHandler::DropDATCache,base::Unretained(this)));
  }
  void GetDebugInfo(const base::ListValue& args) {
    CHECK_EQ(1u,args.size());AllowJavascript();
    brave_shields::AdBlockServiceLease lease(*web_ui()->GetWebContents()->GetBrowserContext());
    if(!lease.Get()){RejectJavascriptCallback(args[0],base::Value("AdBlock context unavailable"));return;}
    memory_instrumentation::MemoryInstrumentation::GetInstance()->RequestGlobalDumpForPid(
        base::Process::Current().Pid(),{"malloc"},base::BindOnce(&BraveAdblockInternalsMessageHandler::OnGetMemoryDump,
            weak_.GetWeakPtr(),args[0].GetString(),lease));
  }
  void OnGetMemoryDump(const std::string& id,brave_shields::AdBlockServiceLease lease,
      memory_instrumentation::mojom::RequestOutcome outcome,
      std::unique_ptr<memory_instrumentation::GlobalMemoryDump> dump) {
    if(!IsJavascriptAllowed())return;auto* service=lease.Get();
    if(!service){RejectJavascriptCallback(base::Value(id),base::Value("AdBlock context unavailable"));return;}
    if(outcome!=memory_instrumentation::mojom::RequestOutcome::kSuccess) {
      RejectJavascriptCallback(base::Value(id),base::Value("failed to get dump"));return;
    }
    CHECK(dump);CHECK(!dump->process_dumps().empty());const auto& process=dump->process_dumps().front();
    base::DictValue memory;auto value=process.GetMetric("malloc","size");
    if(value)memory.Set("malloc/size_kb",base::NumberToString(base::ByteSize(*value).InKiB()));
    memory.Set("private_footprint_kb",static_cast<int>(process.os_dump().private_footprint_kb));
    service->GetDebugInfoAsync(base::BindOnce(&BraveAdblockInternalsMessageHandler::OnGetDebugInfo,
        weak_.GetWeakPtr(),id,lease,std::move(memory)));
  }
  void OnGetDebugInfo(const std::string& id,brave_shields::AdBlockServiceLease lease,base::DictValue memory,
      std::pair<base::DictValue,base::DictValue> debug) {
    if(!IsJavascriptAllowed())return;auto* service=lease.Get();
    if(!service){RejectJavascriptCallback(base::Value(id),base::Value("AdBlock context unavailable"));return;}
    base::DictValue result;result.Set("default_engine",std::move(debug.first));result.Set("additional_engine",std::move(debug.second));
    result.Set("memory",std::move(memory));result.Set("debug_mode",service->IsDebugMode());ResolveJavascriptCallback(base::Value(id),result);
  }
  void DiscardRegex(const base::ListValue& args) {
    CHECK_EQ(1u,args.size());uint64_t id=0;if(!base::StringToUint64(args[0].GetString(),&id))return;
    if(auto* service=Service())service->DiscardRegex(id);
  }
  void SetDebugMode(const base::ListValue& args) {
    CHECK_EQ(1u,args.size());if(auto* service=Service()){service->SetDebugMode(args[0].GetBool());service->DropDATCache();}
  }
  void DropDATCache(const base::ListValue& args){CHECK_EQ(0u,args.size());if(auto* service=Service())service->DropDATCache();}
  base::WeakPtrFactory<BraveAdblockInternalsMessageHandler> weak_{this};
};
}  // namespace
BraveAdblockInternalsUI::BraveAdblockInternalsUI(content::WebUI* ui):content::WebUIController(ui) {
  CreateAndAddWebUIDataSource(ui,kAdblockInternalsHost,kBraveAdblockInternalsGenerated,IDR_BRAVE_ADBLOCK_INTERNALS_HTML);
  ui->AddMessageHandler(std::make_unique<BraveAdblockInternalsMessageHandler>());
}
BraveAdblockInternalsUI::~BraveAdblockInternalsUI()=default;
