// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_INPUT_H_
#define BRAVE_ALLOY_GUEST_STREAM_INPUT_H_
#include <set>
#include <functional>
#include "brave/alloy/guest/stream/interaction.h"
#include "cef/include/alloy/alloy_guest_stream.h"
#include "cef/include/cef_browser.h"
namespace alloy::guest::stream {
CefRefPtr<CefDictionaryValue> ParseDetails(const std::string& json);
std::string WriteDetails(CefRefPtr<CefDictionaryValue> dictionary);
CefRefPtr<AlloyGuestInteraction> MakeInteraction(Owner owner, InteractionData data);
bool ValidUtf16(const CefString& text);
class Input {
 public:
  using Receipt = std::function<void(Owner, InteractionData)>;
  void Reset() { touches_.clear(); }
  void Dispatch(Owner owner, uint64_t generation, const CefRect& view,
                bool popup_visible, const CefRect& popup,
                CefRefPtr<CefBrowser> browser, InteractionData input,
                std::function<uint64_t()> clock, Receipt receipt);
 private:
  uint64_t last_id_ = 0;
  std::set<int> touches_;
};
}  // namespace alloy::guest::stream
#endif
