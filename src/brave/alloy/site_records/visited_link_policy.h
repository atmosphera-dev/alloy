// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_VISITED_LINK_POLICY_H_
#define BRAVE_ALLOY_SITE_RECORDS_VISITED_LINK_POLICY_H_

#include "brave/alloy/site_records/request_context_policy.h"

namespace alloy::site_records {
// Admission for native visited-link projections, not another storage authority.
// Capture the original context capability before a writer initializes. A lock
// or shutdown revokes it; retaining a writer cannot obtain replacement rights.
class VisitedLinkPolicy {
 public:
  VisitedLinkPolicy(const base::SupportsUserData* context, bool off_the_record)
      : configured_(context && RequestContextPolicy::Get(*context)),
        upstream_admission_(!off_the_record),
        capability_(configured_ ? RequestContextPolicy::Get(*context)->StorageCapability(
                                     StorePurpose::kVisitedLinks)
                                : RecordCapability()) {}

  bool CanWriteMemory() const {
    return configured_ ? capability_.Allows(StorePurpose::kVisitedLinks,
                                            StorageAccess::kWriteMemory)
                       : upstream_admission_;
  }
  bool CanPersist(bool requested) const {
    // Configured native history/visited-link tables are RAM projections. Real
    // encrypted record files remain owned by their existing sharded adapters.
    return requested && !configured_ && upstream_admission_;
  }

 private:
  const bool configured_;
  const bool upstream_admission_;
  const RecordCapability capability_;
};
}  // namespace alloy::site_records
#endif
