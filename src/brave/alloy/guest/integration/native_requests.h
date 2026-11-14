// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_NATIVE_REQUESTS_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_NATIVE_REQUESTS_H_

#include "base/memory/weak_ptr.h"
#include "brave/alloy/guest/integration/request_owner.h"
#include "brave/alloy/guest/providers.h"
#include "brave/alloy/guest/stream/interaction.h"
#include "content/public/browser/alloy_guest_integration.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/frame_tree_node_id.h"

namespace alloy::guest::integration {

class NativeRequests : public content::WebContentsObserver {
 public:
  using Event = std::function<void(Owner, std::string, std::string)>;
  using Owns = std::function<bool(Owner)>;
  using UiDispatch = std::function<void(Owner, const stream::InteractionData&,
                                         std::function<void(std::string)>)>;
  NativeRequests(Owner owner, Event event, Owns owns,
                   std::function<uint64_t()> clock,
                   std::function<uint64_t()> generation, UiDispatch ui);
  ~NativeRequests() override;
  void Bind(content::WebContents* contents);
  void SetOwner(Owner owner);
  void Call(Owner owner, const std::string& operation,
            const std::string& payload, ProviderReply reply);
  void Sweep();
  void Notify(std::string operation, base::DictValue details);
  void Close();

 private:
  void DidStartNavigation(content::NavigationHandle* navigation) override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void WebContentsDestroyed() override;
  void Request(content::RenderFrameHost* original,
                content::AlloyGuestIntegration::Operation operation,
                content::AlloyGuestIntegrationData data,
                content::AlloyGuestIntegration::Reply reply);
  base::RepeatingCallback<bool()> Capture(content::RenderFrameHost* original,
                                             bool document_only);
  Owner owner_;
  const Event event_;
  const Owns owns_;
  const std::function<uint64_t()> clock_;
  const std::function<uint64_t()> generation_;
  const UiDispatch ui_;
  bool attached_ = false;
  bool closed_ = false;
  RequestOwner requests_;
  struct PendingInfo;
  struct DocumentCapture;
  std::vector<std::weak_ptr<DocumentCapture>> document_captures_;
  std::map<int64_t, content::FrameTreeNodeId> navigating_frames_;
  std::map<uint64_t, std::shared_ptr<PendingInfo>> pending_;
  uint64_t last_action_ = 0;
  base::WeakPtr<content::WebContents> contents_;
  base::WeakPtrFactory<NativeRequests> weak_{this};
};

}  // namespace alloy::guest::integration
#endif
