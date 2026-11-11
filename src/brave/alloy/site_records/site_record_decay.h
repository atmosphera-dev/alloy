// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_SITE_RECORD_DECAY_H_
#define BRAVE_ALLOY_SITE_RECORDS_SITE_RECORD_DECAY_H_

#include "brave/alloy/site_records/record_file_system.h"

namespace alloy::site_records {
enum class DecayTimebase { kSinceLastVisit = 0 };
struct DecayPolicy {
  int64_t period_us = 0;
  DecayTimebase timebase = DecayTimebase::kSinceLastVisit;
};
int64_t DecayNowMicros();
FileStatus ReadDecayPolicy(RecordFileSystem& files, DecayPolicy* policy);
FileStatus WriteDecayPolicy(RecordFileSystem& files, DecayPolicy policy);
// Future timestamps are durably clamped so a clock correction cannot give a
// record an indefinitely moving deadline. Never-visited records use creation.
FileStatus NextDecayedRecord(RecordFileSystem& files, int64_t now_us,
                            std::optional<RecordInfo>* record);
// Before publication of a newly unlocked coordinator, no native contexts exist.
// Use the same settings/reference + R13 lifecycle without a native UI barrier.
FileStatus DecayBeforeAdmission(std::shared_ptr<RecordFileSystem> files);
}  // namespace alloy::site_records
#endif
