// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/projection.h"
#include <utility>
#include "base/strings/string_number_conversions.h"
namespace alloy::guest::accessibility {
Projection::Result Projection::Receive(const base::DictValue& update) {
  const auto result = model_.Apply(update);
  if (result == Model::Result::kOld) return Result::kIgnored;
  if (result == Model::Result::kResync) {
    pending_.reset(); coalesced_ = 0;
    if (resync_pending_ && update.FindBool("snapshot").value_or(false)) {
      resync_pending_ = false; Fail(); return Result::kUnavailable;
    }
    if (resync_pending_) return Result::kIgnored;
    resync_pending_ = true; return Result::kResync;
  }
  resync_pending_ = false;
  if (pending_) {
    auto snapshot = model_.Snapshot(update);
    snapshot.Set("coalesced_updates", base::NumberToString(++coalesced_));
    pending_ = std::move(snapshot);
  } else {
    coalesced_ = 0; pending_ = update.Clone();
  }
  return Result::kQueued;
}
bool Projection::RequestResync() {
  model_.Clear(); pending_.reset(); coalesced_ = 0;
  if (resync_pending_) return false;
  resync_pending_ = true; return true;
}
void Projection::Fail() { model_.Clear(); pending_.reset(); coalesced_ = 0; unavailable_ = true; }
void Projection::Clear() { model_.Clear(); pending_.reset(); coalesced_ = 0; resync_pending_ = unavailable_ = false; }
bool Projection::TakeUnavailable() { return std::exchange(unavailable_, false); }
std::optional<base::DictValue> Projection::Take() {
  auto result = std::move(pending_); pending_.reset(); coalesced_ = 0; return result;
}
}
