// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef NET_DNS_REQUEST_ANNOTATION_H_
#define NET_DNS_REQUEST_ANNOTATION_H_

#include <string_view>
#include "net/base/net_errors.h"
#include "net/base/network_delegate.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request_context.h"

namespace net {
// Permission is re-evaluated by the owning context; metadata is not an allow
// boolean, and absence of an admission owner never grants native DNS access.
inline int CheckDnsRequestAdmission(URLRequestContext* context,
                                    std::string_view endpoint,
                                    int originating_annotation_hash,
                                    bool* allow_owned_doh_bootstrap = nullptr) {
  if (!context || !context->network_delegate())
    return ERR_NETWORK_ACCESS_DENIED;
  bool ignored = false;
  return context->network_delegate()->NotifyBeforeResolveHost(endpoint,
      CreateMutableNetworkTrafficAnnotationTag(originating_annotation_hash),
      allow_owned_doh_bootstrap ? allow_owned_doh_bootstrap : &ignored);
}
}  // namespace net
#endif
