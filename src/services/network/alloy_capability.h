// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef SERVICES_NETWORK_ALLOY_CAPABILITY_H_
#define SERVICES_NETWORK_ALLOY_CAPABILITY_H_
#include <optional>
#include "services/network/alloy_guard.h"

namespace network {
// An owned transport retains its creator's label, not a remembered boolean.
// Every operation re-enters Decide with current policy and its owner's label.
class AlloyCapability {
 public:
  AlloyCapability() : guard_(base::MakeRefCounted<AlloyGuard>()) {}
  AlloyCapability(scoped_refptr<AlloyGuard> guard, int annotation)
      : guard_(std::move(guard)), annotation_(annotation) {}
  int originating_annotation_hash() const { return annotation_; }
  bool Check(std::string_view endpoint, std::string_view source,
             std::optional<int> child_annotation = std::nullopt) const {
    return guard_->Decide(annotation_ ? child_annotation.value_or(annotation_) : 0, endpoint,
                          source, AlloyGuard::Operation::kRequest,
                          annotation_).allowed;
  }
 private:
  scoped_refptr<AlloyGuard> guard_;
  int annotation_ = 0;
};
}  // namespace network
#endif
