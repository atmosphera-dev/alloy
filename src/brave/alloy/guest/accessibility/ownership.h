// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_ACCESSIBILITY_OWNERSHIP_H_
#define BRAVE_ALLOY_GUEST_ACCESSIBILITY_OWNERSHIP_H_
#include <functional>
#include <set>
#include "brave/alloy/guest/accessibility/tree_stream.h"
#include "brave/alloy/guest/session/extensions.h"
#include "cef/include/cef_browser.h"
#include "content/public/browser/web_contents_observer.h"
namespace alloy::guest::accessibility {
struct Geometry { uint64_t generation; double scale; bool ready; int pixel_width; int pixel_height; };
using Event = std::function<void(Owner, std::string, std::string)>;
// Owns only this view's native frame trees. The session injects ownership,
// geometry and output; no native object crosses the process boundary.
class Connector final : public content::WebContentsObserver {
 public:
  Connector(Owner owner, CefRefPtr<CefBrowser> browser,
            std::function<Geometry()> geometry, std::function<uint64_t()> clock,
            Event event);
  ~Connector() override;
  void SetOwner(Owner owner);
  void GeometryChanged();
  void Retire();
  void Call(Owner owner, const std::string& operation, const std::string& payload,
            ProviderReply reply);
  void AccessibilityEventReceived(const ui::AXUpdatesAndEvents& details) override;
  void AccessibilityLocationChangesReceived(const ui::AXTreeID& id,
      ui::AXLocationAndScrollUpdates& details) override;
  void DidStartNavigation(content::NavigationHandle* navigation) override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void RenderFrameDeleted(content::RenderFrameHost* frame) override;
  void RenderFrameHostChanged(content::RenderFrameHost* old_frame,
                              content::RenderFrameHost* new_frame) override;
  void RenderFrameHostStateChanged(content::RenderFrameHost*,
      content::RenderFrameHost::LifecycleState, content::RenderFrameHost::LifecycleState) override;
  void OnWebContentsFocused(content::RenderWidgetHost*) override;
  void OnWebContentsLostFocus(content::RenderWidgetHost*) override;
 private:
  content::RenderFrameHost* Frame(const ui::AXTreeID& id) const;
  void Publish(bool snapshot, const ui::AXTreeID& id = ui::AXTreeIDUnknown(),
               const ui::AXUpdatesAndEvents* events = nullptr);
  void ResetNative();
  void Remove(content::RenderFrameHost* frame);
  void Action(const std::string& payload);
  Owner owner_;
  CefRefPtr<CefBrowser> browser_;
  const std::function<Geometry()> geometry_;
  const std::function<uint64_t()> clock_;
  const Event event_;
  TreeStream trees_;
  std::set<content::FrameTreeNodeId> pending_frames_;
  uint64_t sequence_ = 0, last_action_ = 0;
  bool started_ = false, retired_ = false;
};
}  // namespace alloy::guest::accessibility
#endif
