// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/ownership.h"
#include <algorithm>
#include <limits>
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "brave/alloy/guest/accessibility/ax_actions.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_event_intent.h"
#include "ui/accessibility/ax_node.h"

namespace alloy::guest::accessibility {
Connector::Connector(Owner owner, CefRefPtr<CefBrowser> browser,
                       std::function<Geometry()> geometry,
                       std::function<uint64_t()> clock, Event event)
    : content::WebContentsObserver(CefBrowserHostBase::FromBrowser(browser)->GetWebContents()),
      owner_(owner), browser_(browser), geometry_(std::move(geometry)),
      clock_(std::move(clock)), event_(std::move(event)) {}
Connector::~Connector() = default;
void Connector::SetOwner(Owner owner) {
  if (retired_ || owner == owner_) return;
  owner_ = owner;
  trees_.Clear(); pending_frames_.clear();
  if (started_) { Publish(true); ResetNative(); }
}
void Connector::GeometryChanged() {
  if (!started_ || retired_) return;
  trees_.Clear(); Publish(true); ResetNative();
}
void Connector::Retire() {
  retired_ = true; trees_.Clear(); Observe(nullptr); browser_ = nullptr;
}
content::RenderFrameHost* Connector::Frame(const ui::AXTreeID& id) const {
  auto* frame = content::RenderFrameHost::FromAXTreeID(id);
  if (!(!retired_ && web_contents() && geometry_().ready && frame && frame->IsActive() &&
      frame->IsRenderFrameLive() && frame->GetAXTreeID() == id &&
      frame->GetMainFrame() == web_contents()->GetPrimaryMainFrame() &&
      content::WebContents::FromRenderFrameHost(frame) == web_contents())) return nullptr;
  for (auto* ancestor = frame; ancestor; ancestor = ancestor->GetParent())
    if (pending_frames_.contains(ancestor->GetFrameTreeNodeId())) return nullptr;
  return frame;
}
void Connector::ResetNative() {
  if (!browser_ || retired_) return;
  // Pinned CEF selects kAXModeWebContentsOnly for OSR. It deliberately does
  // not create BrowserAccessibilityManager/platform accessible objects.
  browser_->GetHost()->SetAccessibilityState(STATE_DISABLED);
  browser_->GetHost()->SetAccessibilityState(STATE_ENABLED);
}
void Connector::Call(Owner owner, const std::string& operation,
                       const std::string& payload, ProviderReply reply) {
  if (retired_ || owner != owner_ || !browser_) { reply(ProviderResult::kCancelled, {}); return; }
  if (operation == "start") {
    if (started_ || !payload.empty()) { reply(ProviderResult::kFailed, {}); return; }
    started_ = true; Publish(true); ResetNative();
  } else if (operation == "resync") {
    if (!started_ || !payload.empty()) { reply(ProviderResult::kFailed, {}); return; }
    Publish(true);
  } else if (operation == "action") {
    if (!started_) { reply(ProviderResult::kFailed, {}); return; }
    Action(payload);
  } else { reply(ProviderResult::kUnavailable, {}); return; }
  reply(ProviderResult::kOk, {});
}
void Connector::Publish(bool snapshot, const ui::AXTreeID& id,
                         const ui::AXUpdatesAndEvents* events) {
  if (retired_ || sequence_ == std::numeric_limits<uint64_t>::max()) return;
  // The live native frame registry, not a late deletion callback's current ID,
  // owns retirement. This also covers inactive/BFCache frames and descendants
  // hidden by a pending ancestor navigation.
  trees_.Prune([this](const ui::AXTreeID& tree) { return Frame(tree) != nullptr; });
  auto geometry = geometry_();
  auto* main_frame = web_contents() ? web_contents()->GetPrimaryMainFrame() : nullptr;
  const auto root_tree = main_frame ? main_frame->GetAXTreeID() : ui::AXTreeIDUnknown();
  const auto* native_tree = trees_.Find(root_tree);
  const auto* native_root = native_tree ? native_tree->root() : nullptr;
  // Blink's bounds are layout/device pixels on this Linux pin. Match the root
  // viewport before applying M3's inverse scale; do not relabel old-scale AX
  // data while new visual properties are still in flight. The raw root bounds
  // exclude the separate pinch/visual-viewport transform, which remains applied
  // by AXTree's coordinate composition.
  const bool bounds_ready = native_root && geometry.scale > 0 &&
      native_root->data().relative_bounds.bounds.width() == geometry.pixel_width &&
      native_root->data().relative_bounds.bounds.height() == geometry.pixel_height;
  auto body = snapshot ? trees_.Snapshot(geometry.scale, bounds_ready)
                       : trees_.Delta(id, geometry.scale, bounds_ready);
  body.Set("document", base::NumberToString(owner_.document));
  body.Set("geometry", base::NumberToString(geometry.generation));
  body.Set("scale", geometry.scale);
  body.Set("root_tree", root_tree == ui::AXTreeIDUnknown() ? std::string() : root_tree.ToString());
  auto* native_view = web_contents() ? web_contents()->GetRenderWidgetHostView() : nullptr;
  body.Set("view_focused", native_view && native_view->AccessibilityHasFocus());
  body.Set("sequence", base::NumberToString(++sequence_));
  body.Set("snapshot", snapshot);
  body.Set("capture_us", base::NumberToString(clock_()));
  base::ListValue notifications;
  if (events) for (const auto& event : events->events) {
    base::DictValue notification;
    notification.Set("tree", id.ToString()); notification.Set("node", event.id);
    notification.Set("kind", ui::ToString(event.event_type));
    notification.Set("action_request", event.action_request_id);
    notification.Set("from", ui::ToString(event.event_from));
    notification.Set("from_action", ui::ToString(event.event_from_action));
    base::ListValue intents;
    for (const auto& intent : event.event_intents) {
      base::DictValue details;
      details.Set("command", ui::ToString(intent.command));
      details.Set("input_type", ui::ToString(intent.input_event_type));
      details.Set("boundary", ui::ToString(intent.text_boundary));
      details.Set("direction", ui::ToString(intent.move_direction));
      intents.Append(std::move(details));
    }
    notification.Set("intents", std::move(intents));
    notifications.Append(std::move(notification));
  }
  body.Set("events", std::move(notifications));
  auto json = base::WriteJson(body);
  if (!json) { event_(owner_, "unavailable", "invalid accessibility values"); return; }
  // Frame trees are not limited to the envelope's URL-sized field. Transfer one
  // atomic semantic update in checked parts on the existing ordered route.
  for (size_t offset = 0; offset < json->size();) {
    const size_t count = std::min<size_t>(ALLOY_GUEST_CONTROL_CHUNK_SIZE, json->size() - offset);
    std::string part;
    if (!PackFields({base::NumberToString(sequence_), base::NumberToString(json->size()),
                      base::NumberToString(offset), json->substr(offset, count)}, &part)) {
      event_(owner_, "unavailable", "invalid accessibility part"); return;
    }
    event_(owner_, "update-part", std::move(part));
    offset += count;
  }
}
void Connector::AccessibilityEventReceived(const ui::AXUpdatesAndEvents& details) {
  if (!started_ || !Frame(details.ax_tree_id)) return;
  if (!trees_.Update(details)) { trees_.Clear(); Publish(true); ResetNative(); return; }
  // A parent mutation can move a whole child frame, so publish a complete
  // snapshot when cross-frame bounds may have changed. Native events stay live.
  Publish(false, details.ax_tree_id, &details);
}
void Connector::AccessibilityLocationChangesReceived(const ui::AXTreeID& id,
    ui::AXLocationAndScrollUpdates& details) {
  if (!started_ || !Frame(id)) return;
  if (!trees_.Location(id, details)) { trees_.Clear(); Publish(true); ResetNative(); return; }
  Publish(true);
}
void Connector::Remove(content::RenderFrameHost* frame) {
  if (!frame) return;
  auto id = frame->GetAXTreeID();
  trees_.Remove(id);
  if (started_ && !retired_) Publish(true);
}
void Connector::DidStartNavigation(content::NavigationHandle* navigation) {
  if (navigation->IsSameDocument()) return;
  auto node = navigation->GetFrameTreeNodeId();
  pending_frames_.insert(node);
  if (web_contents()) web_contents()->GetPrimaryMainFrame()->ForEachRenderFrameHost(
      [this, node](content::RenderFrameHost* frame) {
    if (frame->GetFrameTreeNodeId() != node) return;
    frame->ForEachRenderFrameHost([this](content::RenderFrameHost* descendant) {
      if (content::WebContents::FromRenderFrameHost(descendant) == web_contents())
        trees_.Remove(descendant->GetAXTreeID());
    });
  });
  if (started_ && !retired_) Publish(true);
}
void Connector::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (navigation->IsSameDocument()) return;
  pending_frames_.erase(navigation->GetFrameTreeNodeId());
  if (started_ && !retired_) { trees_.Clear(); Publish(true); ResetNative(); }
}
void Connector::RenderFrameDeleted(content::RenderFrameHost* frame) { Remove(frame); }
void Connector::RenderFrameHostChanged(content::RenderFrameHost* old_frame,
                                       content::RenderFrameHost*) { Remove(old_frame); }
void Connector::RenderFrameHostStateChanged(content::RenderFrameHost*,
    content::RenderFrameHost::LifecycleState, content::RenderFrameHost::LifecycleState) {
  if (started_ && !retired_) Publish(true);
}
void Connector::OnWebContentsFocused(content::RenderWidgetHost*) { if (started_ && !retired_) Publish(true); }
void Connector::OnWebContentsLostFocus(content::RenderWidgetHost*) { if (started_ && !retired_) Publish(true); }
void Connector::Action(const std::string& payload) {
  stream::InteractionData input;
  if (!stream::UnpackInteraction(payload, &input) || !stream::FitsInteraction(input)) return;
  std::string reason;
  auto details = base::JSONReader::ReadDict(input.details, base::JSON_PARSE_RFC);
  auto action = details ? ParseAction(*details) : std::nullopt;
  auto geometry = geometry_();
  uint64_t tree_generation = 0, node_generation = 0;
  const auto* tree_epoch = details ? details->FindString("tree_generation") : nullptr;
  const auto* node_epoch = details ? details->FindString("node_generation") : nullptr;
  if (input.id <= last_action_) reason = "event-order";
  else if (input.document != owner_.document || input.generation != geometry.generation || !geometry.ready)
    reason = "stale-generation";
  else if (input.kind != "ax" || input.surface || input.dispatch_us || !input.outcome.empty() ||
           !action || !tree_epoch || !node_epoch ||
           !base::StringToUint64(*tree_epoch, &tree_generation) ||
           !base::StringToUint64(*node_epoch, &node_generation)) reason = "invalid";
  else if (!trees_.Resolve(action->target_tree_id.ToString(), tree_generation,
                            action->target_node_id, node_generation)) reason = "stale-node";
  auto* frame = reason.empty() ? Frame(action->target_tree_id) : nullptr;
  if (reason.empty() && !frame) reason = "retired-frame";
  last_action_ = std::max(last_action_, input.id);
  input.dispatch_us = clock_();
  if (reason.empty()) {
    frame->AccessibilityPerformAction(*action);
    input.outcome = "accepted";
  } else input.outcome = "rejected:" + reason;
  std::string receipt;
  if (stream::PackInteraction(input, &receipt)) event_(owner_, stream::kInteractionOperation, std::move(receipt));
}
}  // namespace alloy::guest::accessibility
