// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_ACCESSIBILITY_MODEL_H_
#define BRAVE_ALLOY_GUEST_ACCESSIBILITY_MODEL_H_
#include <cstdint>
#include <map>
#include <string>
#include "base/values.h"
namespace alloy::guest::accessibility {
// Portable app-side validation. A gap invalidates action admission until a new
// complete snapshot arrives. Apply is transactional, including relationships.
class Model {
 public:
  enum class Result { kApplied, kOld, kResync };
  Result Apply(const base::DictValue& update);
  base::DictValue Snapshot(const base::DictValue& context) const;
  void Clear();
  bool ready() const { return ready_; }
  uint64_t sequence() const { return sequence_; }
 private:
  bool ready_ = false;
  uint64_t sequence_ = 0;
  std::map<std::string, base::DictValue> trees_;
};
bool ValidateTree(const base::DictValue& tree);

// Checked atomic multipart delivery; peer-supplied sizes never cause an eager
// allocation. The existing envelope bounds each part, not the whole forest.
class UpdateParts {
 public:
  enum class Result { kPending, kReady, kResync };
  Result Add(uint64_t sequence, uint64_t total, uint64_t offset,
             const std::string& part, std::string* complete);
  void Clear();
 private:
  std::string partial_;
  uint64_t sequence_ = 0, total_ = 0;
};
}  // namespace alloy::guest::accessibility
#endif
