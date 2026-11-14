// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_MENUS_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_MENUS_H_
#include <functional>
#include <memory>
#include "cef/include/cef_context_menu_handler.h"
#include "content/public/browser/alloy_guest_integration.h"
#include "content/public/browser/weak_document_ptr.h"
namespace alloy::guest::integration {
class Menus final : public CefContextMenuHandler {
 public:
  explicit Menus(std::function<uint64_t()> generation);
  ~Menus() override;
  bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
      CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel>,
      CefRefPtr<CefRunContextMenuCallback>) override;
  bool OnContextMenuCommand(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
      CefRefPtr<CefContextMenuParams>, int, EventFlags) override;
  void OnContextMenuDismissed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) override;
  bool RunQuickMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefPoint&,
      const CefSize&, QuickMenuEditStateFlags,
      CefRefPtr<CefRunQuickMenuCallback>) override;
  bool OnQuickMenuCommand(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
      EventFlags) override;
  void OnQuickMenuDismissed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) override;
 private:
  struct Menu;
  bool Offer(CefRefPtr<CefFrame>, bool quick,
      content::AlloyGuestIntegrationData,
      std::function<void(int, cef_event_flags_t)> finish);
  bool Dispatch(int command);
  void Dismiss();
  const std::function<uint64_t()> generation_;
  std::shared_ptr<Menu> current_;
  IMPLEMENT_REFCOUNTING(Menus);
};
}  // namespace alloy::guest::integration
#endif
