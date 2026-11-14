// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/interactions.h"
#include <limits>
#include <utility>
namespace alloy::guest {
void Interactions::BeginView(uint64_t view) { ids_.try_emplace(view, 1); }
std::optional<uint64_t> Interactions::Next(uint64_t view) const {
  auto found = ids_.find(view);
  if (found == ids_.end() || found->second == std::numeric_limits<uint64_t>::max()) return std::nullopt;
  return found->second;
}
bool Interactions::Track(Feature feature, Owner owner, stream::InteractionData input) {
  auto next = Next(owner.view);
  if (!next || input.id != *next || input.document != owner.document) return false;
  ++ids_.find(owner.view)->second;
  pending_.emplace(std::make_pair(owner.view, input.id), Action{feature, owner, std::move(input)});
  return true;
}
Interactions::Receipt Interactions::Receive(Feature feature, Owner owner,
                                             const stream::InteractionData& value) {
  auto found = pending_.find({owner.view, value.id});
  if (found == pending_.end()) return Receipt::kMissing;
  const auto& expected = found->second;
  if (value.outcome.empty() || expected.feature != feature || expected.owner != owner ||
      expected.input.document != value.document || expected.input.generation != value.generation ||
      expected.input.surface != value.surface || expected.input.capture_us != value.capture_us ||
      expected.input.source != value.source || expected.input.kind != value.kind ||
      expected.input.details != value.details) return Receipt::kInvalid;
  pending_.erase(found);
  return Receipt::kReady;
}
std::optional<Interactions::Action> Interactions::Complete(uint64_t view, uint64_t id) {
  auto found = pending_.find({view, id});
  if (found == pending_.end()) return std::nullopt;
  auto action = std::move(found->second); pending_.erase(found);
  return action;
}
void Interactions::RetireView(uint64_t view) {
  ids_.erase(view);
  std::erase_if(pending_, [view](const auto& action) { return action.first.first == view; });
}
}  // namespace alloy::guest
