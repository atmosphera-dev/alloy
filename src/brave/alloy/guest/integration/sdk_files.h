// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_SDK_FILES_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_SDK_FILES_H_
#include <functional>
#include <memory>
#include "include/alloy/alloy_guest_integration.h"
// Synthetic app-owned filesystem; never linked into the product provider.
class SyntheticGuestFiles {
 public:
  using Event = std::function<void(const char*, CefRefPtr<CefDictionaryValue>)>;
  explicit SyntheticGuestFiles(Event event);
  ~SyntheticGuestFiles();
  void Select(CefRefPtr<AlloyGuestFileSelection> request);
  void Configure(CefRefPtr<CefDictionaryValue> options);
  void Ended(uint64_t id);
  void Release();
  CefRefPtr<CefDictionaryValue> Snapshot();
 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};
#endif
