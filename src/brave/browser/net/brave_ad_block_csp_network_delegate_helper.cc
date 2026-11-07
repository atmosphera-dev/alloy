/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "brave/browser/net/brave_ad_block_csp_network_delegate_helper.h"
#include <optional>
#include <string>
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "brave/browser/brave_shields/profile_adblock_service_factory.h"
#include "brave/browser/net/url_context.h"
#include "brave/components/brave_shields/content/browser/ad_block_engine_wrapper.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"
#include "brave/components/brave_shields/core/browser/ad_block_service_helper.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "url/gurl.h"
namespace brave {
std::optional<std::string> GetCspDirectivesOnTaskRunner(const url::Origin& first_party_origin,
    const GURL& url,blink::mojom::ResourceType type,const std::string& method,
    std::optional<std::string> original,brave_shields::AdBlockEngineWrapper* wrapper) {
  auto directives=wrapper->GetCspDirectives(url,type,first_party_origin,method);
  brave_shields::MergeCspDirectiveInto(original,&directives);return directives;
}
template<template<typename> class T>
int OnHeadersReceived_AdBlockCspWork(const net::HttpResponseHeaders* response_headers,
    scoped_refptr<net::HttpResponseHeaders>* override_headers,GURL*,const ResponseCallback& next,T<BraveRequestInfo> ctx) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);DCHECK(ctx);
  if(!response_headers||!ctx->allow_brave_shields()||ctx->allow_ads())return net::OK;
  if(ctx->resource_type()!=blink::mojom::ResourceType::kMainFrame&&
      ctx->resource_type()!=blink::mojom::ResourceType::kSubFrame)return net::OK;
  auto* context=ctx->browser_context();if(!context)return net::ERR_ABORTED;
  brave_shields::AdBlockServiceLease lease(*context);auto* service=lease.Get();if(!service)return net::ERR_ABORTED;
  if(!*override_headers)*override_headers=new net::HttpResponseHeaders(response_headers->raw_headers());
  auto original=(*override_headers)->GetNormalizedHeader("Content-Security-Policy");
  (*override_headers)->RemoveHeader("Content-Security-Policy");
  const bool use_initiator=ctx->request_initiator()&&
      (ctx->resource_type()!=blink::mojom::ResourceType::kMainFrame||ctx->request_initiator()->opaque());
  const auto origin=use_initiator?*ctx->request_initiator():url::Origin::Create(ctx->request_url());
  const bool accepted=service->AsyncCallAndReplyWithResult<std::optional<std::string>>(
      base::BindOnce(&GetCspDirectivesOnTaskRunner,origin,ctx->request_url(),ctx->resource_type(),ctx->method(),std::move(original)),
      base::BindOnce([](T<BraveRequestInfo> ctx,brave_shields::AdBlockServiceLease lease,
          ResponseCallback next,scoped_refptr<net::HttpResponseHeaders> headers,std::optional<std::string> directives) {
        DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
        if(!ctx){next.Run();return;}
        if(!lease.Get()){ctx->set_pending_error(net::ERR_ABORTED);next.Run();return;}
        if(directives)headers->AddHeader("Content-Security-Policy",*directives);
        next.Run();
      },ctx,lease,next,*override_headers));
  return accepted?net::ERR_IO_PENDING:net::ERR_ABORTED;
}
template int OnHeadersReceived_AdBlockCspWork<std::shared_ptr>(const net::HttpResponseHeaders*,scoped_refptr<net::HttpResponseHeaders>*,GURL*,const ResponseCallback&,std::shared_ptr<BraveRequestInfo>);
template int OnHeadersReceived_AdBlockCspWork<base::WeakPtr>(const net::HttpResponseHeaders*,scoped_refptr<net::HttpResponseHeaders>*,GURL*,const ResponseCallback&,base::WeakPtr<BraveRequestInfo>);
}  // namespace brave
