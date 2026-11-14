// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/clipboard.h"
#include "base/functional/bind.h"
#include "cef/libcef/browser/frame_host_impl.h"
namespace alloy::guest::integration {
bool IsEditCommand(int command) {
  switch (command) {
    case MENU_ID_UNDO: case MENU_ID_REDO: case MENU_ID_CUT: case MENU_ID_COPY:
    case MENU_ID_PASTE: case MENU_ID_PASTE_MATCH_STYLE: case MENU_ID_DELETE:
    case MENU_ID_SELECT_ALL: return true;
    default: return false;
  }
}
void DispatchEditCaptured(CefRefPtr<CefFrame> frame, int command,
    std::function<bool()> admitted, std::function<void(bool)> completion,
    std::optional<base::UnguessableToken> edit_context) {
  if (!frame || !frame->IsValid() || !admitted()) { completion(false); return; }
  const char* name = nullptr;
  switch (command) {
    case MENU_ID_UNDO: name = "Undo"; break;
    case MENU_ID_REDO: name = "Redo"; break;
    case MENU_ID_CUT: name = "Cut"; break;
    case MENU_ID_COPY: name = "Copy"; break;
    case MENU_ID_PASTE: name = "Paste"; break;
    case MENU_ID_PASTE_MATCH_STYLE: name = "PasteAndMatchStyle"; break;
    case MENU_ID_DELETE: name = "Delete"; break;
    case MENU_ID_SELECT_ALL: name = "SelectAll"; break;
    default: completion(false); return;
  }
  static_cast<CefFrameHostImpl*>(frame.get())->SendGuestCommand(name,
      base::BindRepeating([](std::function<bool()> check) { return check(); }, std::move(admitted)),
      base::BindOnce([](std::function<void(bool)> done, bool accepted) { done(accepted); },
                       std::move(completion)), std::move(edit_context));
}
}  // namespace alloy::guest::integration
