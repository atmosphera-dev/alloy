// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_INTERACTIONS_H_
#define BRAVE_ALLOY_GUEST_SESSION_INTERACTIONS_H_
#include <map>
#include <optional>
#include "brave/alloy/guest/providers.h"
#include "brave/alloy/guest/stream/interaction.h"
namespace alloy::guest {
// Pending action bookkeeping only. The existing Session admits/retires views;
// the client sequence (under its queue mutex) calls this shared tracker for all
// feature producers. No completed action/history is retained.
class Interactions {
 public:
  struct Action { Feature feature; Owner owner; stream::InteractionData input; };
  enum class Receipt { kMissing, kInvalid, kReady };
  void BeginView(uint64_t view);
  std::optional<uint64_t> Next(uint64_t view) const;
  bool Track(Feature feature, Owner owner, stream::InteractionData input);
  Receipt Receive(Feature feature, Owner owner, const stream::InteractionData& receipt);
  std::optional<Action> Complete(uint64_t view, uint64_t id);
  void RetireView(uint64_t view);
 private:
  std::map<uint64_t, uint64_t> ids_;
  std::map<std::pair<uint64_t, uint64_t>, Action> pending_;
};
}  // namespace alloy::guest
#endif
