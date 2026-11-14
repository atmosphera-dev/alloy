// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/popup.h"
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#include "base/strings/string_number_conversions.h"
#include "brave/alloy/guest/session/extensions.h"
namespace alloy::guest::stream {
Popups::~Popups() { Close(); }
void Popups::Add(Owner opener, Owner child, std::string metadata, Attach attach,
                  std::function<void()> deny) {
  pending_.emplace(child.view, Pending{opener, child, std::move(metadata), std::move(attach), std::move(deny)});
}
void Popups::Ready(uint64_t child) {
  auto found = pending_.find(child);
  if (found == pending_.end() || found->second.ready) return;
  found->second.ready = true;
  auto offer = offer_;
  const auto opener = found->second.opener, original = found->second.child;
  auto metadata = found->second.metadata;
  offer(opener, original, std::move(metadata));
}
bool Popups::Call(Owner opener, const std::string& operation, const std::string& payload,
                   ProviderReply reply) {
  if (operation != "window-decision") return false;
  std::vector<std::string> fields;
  uint64_t id = 0; int width = 0, height = 0; double scale = 0;
  if (!UnpackFields(payload, 5, &fields) || !ParseUnsigned(fields[0], &id) ||
      (fields[1] != "0" && fields[1] != "1") || !base::StringToInt(fields[2], &width) ||
      !base::StringToInt(fields[3], &height) || !base::StringToDouble(fields[4], &scale)) {
    reply(ProviderResult::kFailed, {}); return true;
  }
  auto found = pending_.find(id);
  if (found == pending_.end() || found->second.opener != opener || !found->second.ready) {
    reply(ProviderResult::kCancelled, {}); return true;
  }
  if (fields[1] == "1" && (width <= 0 || height <= 0 ||
      !std::isfinite(static_cast<float>(scale)) || static_cast<float>(scale) <= 0 ||
      std::ceil(width * scale) > std::numeric_limits<int>::max() ||
      std::ceil(height * scale) > std::numeric_limits<int>::max())) {
    reply(ProviderResult::kFailed, {}); return true;
  }
  auto pending = std::move(found->second); pending_.erase(found);
  bool accepted = fields[1] == "1" && pending.attach(width, height, scale);
  if (!accepted) pending.deny();
  reply(fields[1] == "0" || accepted ? ProviderResult::kOk : ProviderResult::kFailed, {});
  return true;
}
void Popups::Cancel(Owner opener) {
  std::vector<std::function<void()>> deny;
  for (auto found = pending_.begin(); found != pending_.end();) {
    if (found->second.opener == opener) {
      deny.push_back(std::move(found->second.deny)); found = pending_.erase(found);
    } else { ++found; }
  }
  for (auto& callback : deny) callback();
}
void Popups::Abort(uint64_t child) {
  auto found = pending_.find(child);
  if (found == pending_.end()) return;
  auto deny = std::move(found->second.deny); pending_.erase(found); deny();
}
void Popups::Close() {
  auto pending = std::move(pending_);
  for (auto& [id, value] : pending) value.deny();
}
}  // namespace alloy::guest::stream
