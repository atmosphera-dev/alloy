// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_POPUP_H_
#define BRAVE_ALLOY_GUEST_STREAM_POPUP_H_
#include <functional>
#include <map>
#include <utility>
#include "brave/alloy/guest/providers.h"
namespace alloy::guest::stream {
// Stores the original opener and callbacks to the already-created native
// child. It owns neither session admission nor another request context.
class Popups {
 public:
  using Offer = std::function<void(Owner, Owner, std::string)>;
  using Attach = std::function<bool(int, int, double)>;
  explicit Popups(Offer offer) : offer_(std::move(offer)) {}
  ~Popups();
  void Add(Owner opener, Owner child, std::string metadata, Attach attach,
           std::function<void()> deny);
  void Ready(uint64_t child);
  bool Call(Owner opener, const std::string& operation, const std::string& payload,
             ProviderReply reply);
  void Cancel(Owner opener);
  void Abort(uint64_t child);
  void Close();
 private:
  struct Pending {
    Owner opener, child;
    std::string metadata;
    Attach attach;
    std::function<void()> deny;
    bool ready = false;
  };
  std::map<uint64_t, Pending> pending_;
  const Offer offer_;
};
}  // namespace alloy::guest::stream
#endif
