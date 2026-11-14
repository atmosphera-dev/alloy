// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/menus.h"
#include "brave/alloy/guest/integration/clipboard.h"

#include <set>
#include <utility>
#include <vector>
#include "base/functional/bind.h"
#include "cef/libcef/browser/frame_host_impl.h"
#include "content/public/browser/alloy_document_state.h"

namespace alloy::guest::integration {
namespace {
bool NativeCommand(int command) {
  if (IsEditCommand(command)) return true;
  switch (command) {
    case MENU_ID_BACK: case MENU_ID_FORWARD:
    case MENU_ID_RELOAD: case MENU_ID_RELOAD_NOCACHE: case MENU_ID_STOPLOAD:
      return true;
    default:
      return false;
  }
}
base::ListValue CopyItems(CefRefPtr<CefMenuModel> root) {
  struct Pending { CefRefPtr<CefMenuModel> model; int parent; };
  std::vector<Pending> pending{{root, -1}};
  std::set<CefMenuModel*> seen;
  base::ListValue items;
  while (!pending.empty()) {
    auto next = std::move(pending.back()); pending.pop_back();
    if (!seen.insert(next.model.get()).second) continue;
    for (size_t index = 0; index < next.model->GetCount(); ++index) {
      const int command = next.model->GetCommandIdAt(index);
      auto submenu = next.model->GetSubMenuAt(index);
      base::DictValue item;
      item.Set("command", command);
      item.Set("label", next.model->GetLabelAt(index).ToString());
      item.Set("type", static_cast<int>(next.model->GetTypeAt(index)));
      item.Set("enabled", next.model->IsEnabledAt(index));
      item.Set("checked", next.model->IsCheckedAt(index));
      item.Set("parent", next.parent);
      item.Set("native", !submenu && NativeCommand(command));
      const int parent = static_cast<int>(items.size());
      items.Append(std::move(item));
      if (submenu) pending.push_back({submenu, parent});
    }
  }
  return items;
}
}  // namespace

struct Menus::Menu {
  CefRefPtr<CefFrame> frame;
  content::WeakDocumentPtr document;
  base::UnguessableToken edit_context = base::UnguessableToken::Create();
  bool live = true, handled = false, quick = false;
  std::set<int> enabled, native;
  std::function<void(int, cef_event_flags_t)> finish;
};

Menus::Menus(std::function<uint64_t()> generation) : generation_(std::move(generation)) {}
Menus::~Menus() { Dismiss(); }

bool Menus::Offer(CefRefPtr<CefFrame> frame, bool quick,
                    content::AlloyGuestIntegrationData data,
                    std::function<void(int, cef_event_flags_t)> finish) {
  auto* original = frame ? static_cast<CefFrameHostImpl*>(frame.get())->GetRenderFrameHost() : nullptr;
  auto* bridge = content::AlloyGuestIntegration::Get(original);
  if (!original || !bridge || !content::AlloyDocumentStateAdmitted(original)) {
    finish(-1, EVENTFLAG_NONE); return true;
  }
  if (current_ && current_->live) {
    auto previous = current_;
    previous->finish(-1, EVENTFLAG_NONE);
    previous->live = false;
    current_.reset();
  }
  auto menu = std::make_shared<Menu>();
  menu->frame = frame; menu->document = original->GetWeakDocumentPtr();
  menu->quick = quick; menu->finish = std::move(finish);
  auto* items = data.details.FindList("items");
  if (!items) { menu->finish(-1, EVENTFLAG_NONE); return true; }
  for (const auto& value : *items) {
    if (!value.is_dict()) continue;
    const auto& item = value.GetDict();
    const auto command = item.FindInt("command");
    if (!command || *command < 0 || !item.FindBool("enabled").value_or(false)) continue;
    menu->enabled.insert(*command);
    if (item.FindBool("native").value_or(false)) menu->native.insert(*command);
  }
  current_ = menu;
  const uint64_t generation = generation_();
  data.details.Set("quick", quick);
  data.details.Set("generation", std::to_string(generation));
  data.admitted = base::BindRepeating(
      [](std::shared_ptr<Menu> menu, std::function<uint64_t()> current, uint64_t expected) {
        return menu->live && expected && current() == expected;
      }, menu, generation_, generation);
  data.validate_reply = base::BindRepeating(
      [](std::shared_ptr<Menu> menu, const base::DictValue& reply,
          const std::vector<std::string>& bytes) {
        const auto command = reply.FindInt("command");
        const auto flags = reply.FindInt("flags");
        const auto handled = reply.FindBool("handled");
        if (!bytes.empty() || !menu->live || !command || !flags || *flags < 0 || !handled)
          return false;
        return *command == -1 || (menu->enabled.contains(*command) &&
            (*handled || menu->native.contains(*command)));
      }, menu);
  // Capture the native editor before the app can hold the menu and move focus.
  // This does not block browser UI and does not expose a renderer node pointer.
  static_cast<CefFrameHostImpl*>(frame.get())->CaptureGuestEditContext(
      menu->edit_context, !quick, base::BindOnce(
      [](std::shared_ptr<Menu> menu, content::AlloyGuestIntegrationData data, bool captured) {
        auto* original = menu->document.AsRenderFrameHostIfValid();
        auto* bridge = content::AlloyGuestIntegration::Get(original);
        if (!menu->live) return;
        if (!captured || !bridge || !data.admitted.Run()) {
          menu->finish(-1, EVENTFLAG_NONE); menu->live = false; return;
        }
        bridge->Request(original, content::AlloyGuestIntegration::Operation::kMenu,
            std::move(data), base::BindOnce([](std::shared_ptr<Menu> menu,
          std::optional<content::AlloyGuestIntegrationData> reply) {
        if (!menu->live) return;
        if (!reply) { menu->finish(-1, EVENTFLAG_NONE); menu->live = false; return; }
        menu->handled = reply->details.FindBool("handled").value_or(false);
        menu->finish(reply->details.FindInt("command").value_or(-1),
            static_cast<cef_event_flags_t>(reply->details.FindInt("flags").value_or(0)));
        menu->live = false;
            }, menu));
      }, menu, std::move(data)));
  return true;
}

bool Menus::RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model,
    CefRefPtr<CefRunContextMenuCallback> callback) {
  content::AlloyGuestIntegrationData data;
  data.details.Set("x", params->GetXCoord()); data.details.Set("y", params->GetYCoord());
  data.details.Set("items", CopyItems(model));
  return Offer(frame, false, std::move(data), [callback](int command, cef_event_flags_t flags) {
    if (command < 0) callback->Cancel(); else callback->Continue(command, flags);
  });
}

bool Menus::RunQuickMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
    const CefPoint& point, const CefSize& size, QuickMenuEditStateFlags flags,
    CefRefPtr<CefRunQuickMenuCallback> callback) {
  base::ListValue items;
  for (auto [command, label] : {std::pair{QM_EDITFLAG_CAN_CUT, "Cut"},
         {QM_EDITFLAG_CAN_COPY, "Copy"}, {QM_EDITFLAG_CAN_PASTE, "Paste"},
         {QM_EDITFLAG_CAN_ELLIPSIS, "More"}}) {
    base::DictValue item;
    item.Set("command", static_cast<int>(command)); item.Set("label", label);
    item.Set("enabled", (flags & command) != 0); item.Set("checked", false);
    item.Set("parent", -1); item.Set("type", 1);
    item.Set("native", command != QM_EDITFLAG_CAN_ELLIPSIS);
    items.Append(std::move(item));
  }
  content::AlloyGuestIntegrationData data;
  data.details.Set("x", point.x); data.details.Set("y", point.y);
  data.details.Set("width", size.width); data.details.Set("height", size.height);
  data.details.Set("items", std::move(items));
  return Offer(frame, true, std::move(data), [callback](int command, cef_event_flags_t event_flags) {
    if (command < 0) callback->Cancel(); else callback->Continue(command, event_flags);
  });
}

bool Menus::Dispatch(int command) {
  auto menu = current_;
  auto* frame = menu ? menu->document.AsRenderFrameHostIfValid() : nullptr;
  if (!menu || !menu->live || !frame || !content::AlloyDocumentStateAdmitted(frame)) return true;
  if (menu->handled) return true;
  if (menu->quick) {
    switch (command) {
      case QM_EDITFLAG_CAN_CUT: command = MENU_ID_CUT; break;
      case QM_EDITFLAG_CAN_COPY: command = MENU_ID_COPY; break;
      case QM_EDITFLAG_CAN_PASTE: command = MENU_ID_PASTE; break;
      default: break;
    }
  }
  if (IsEditCommand(command)) {
    const auto document = menu->document;
    DispatchEditCaptured(menu->frame, command,
        [document] { return content::AlloyDocumentStateAdmitted(document.AsRenderFrameHostIfValid()); },
        [](bool) {}, menu->edit_context);
    return true;
  }
  return !NativeCommand(command);
}
bool Menus::OnContextMenuCommand(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
    CefRefPtr<CefContextMenuParams>, int command, EventFlags) { return Dispatch(command); }
bool Menus::OnQuickMenuCommand(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
    int command, EventFlags) { return Dispatch(command); }
void Menus::Dismiss() {
  auto menu = std::move(current_);
  if (menu) {
    menu->live = false;
    static_cast<CefFrameHostImpl*>(menu->frame.get())->ReleaseGuestEditContext(menu->edit_context);
  }
}
void Menus::OnContextMenuDismissed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) { Dismiss(); }
void Menus::OnQuickMenuDismissed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) { Dismiss(); }
}  // namespace alloy::guest::integration
