// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_DRAG_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_DRAG_H_
#include <functional>
#include <memory>
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "brave/alloy/guest/stream/interaction.h"
#include "cef/include/cef_browser.h"
#include "cef/include/cef_drag_data.h"
namespace alloy::guest::integration {
class Actions {
 public:
  using Live = std::function<bool(Owner, uint64_t)>;
  using Point = std::function<bool(uint64_t, uint64_t, uint64_t, int, int, bool, CefMouseEvent*)>;
  using Notify = std::function<void(std::string, base::DictValue)>;
  Actions(Owner owner, Live live, Point point, Notify notify);
  ~Actions();
  void Bind(CefRefPtr<CefBrowser> browser);
  void SetOwner(Owner owner);
  void Reset();
  void Dispatch(Owner original, const stream::InteractionData& action,
                    std::function<void(std::string)> completion);
  bool StartDragging(CefRefPtr<CefDragData> data, int allowed, int x, int y,
                        uint64_t generation);
  void Cursor(int operation);
 private:
  struct Source;
  std::string DispatchNow(Owner original, const stream::InteractionData& action);
  Owner owner_;
  const Live live_;
  const Point point_;
  const Notify notify_;
  CefRefPtr<CefBrowser> browser_;
  bool target_ = false;
  std::shared_ptr<Source> source_;
  base::WeakPtrFactory<Actions> weak_{this};
};
}  // namespace alloy::guest::integration
#endif
