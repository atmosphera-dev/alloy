// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_ACCESSIBILITY_PROJECTION_H_
#define BRAVE_ALLOY_GUEST_ACCESSIBILITY_PROJECTION_H_
#include <optional>
#include "brave/alloy/guest/accessibility/model.h"
namespace alloy::guest::accessibility {
// Serialized by the client adapter. Owns current validated state and at most
// one pending projection, not callback/toolkit objects or completed history.
class Projection {
 public:
  enum class Result { kQueued, kIgnored, kResync, kUnavailable };
  Result Receive(const base::DictValue& update);
  bool RequestResync();
  void Fail();
  void Clear();
  bool ready() const { return model_.ready(); }
  bool pending() const { return pending_.has_value(); }
  bool unavailable() const { return unavailable_; }
  bool TakeUnavailable();
  std::optional<base::DictValue> Take();
 private:
  Model model_;
  std::optional<base::DictValue> pending_;
  uint64_t coalesced_ = 0;
  bool resync_pending_ = false, unavailable_ = false;
};
}
#endif
