// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_record_decay.h"

#include <array>
#include <chrono>

#include "base/functional/bind.h"
#include "brave/alloy/site_records/site_pref_store_io.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/browser/owned_settings_registries.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr char kPolicyFile[] = "site-record-decay";
constexpr auto kPurpose = StorePurpose::kProfileSettings;
}
int64_t DecayNowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
FileStatus ReadDecayPolicy(RecordFileSystem& files, DecayPolicy* policy) {
  std::shared_ptr<RecordFile> file;
  auto status = files.OpenFile(files.ProfileCapability(kPurpose), kPurpose,
                              kPolicyFile, RecordOpen::kOpen, &file);
  if (status == FileStatus::kNotFound) { *policy = {}; return FileStatus::kOk; }
  if (status != FileStatus::kOk) return status;
  RecordFileInfo info;
  status = file->GetInfo(&info);
  if (status != FileStatus::kOk) return status;
  if (info.length != 16) return FileStatus::kIntegrity;
  std::array<uint8_t, 16> bytes{};
  size_t read = 0;
  status = file->Read(0, bytes, &read);
  if (status != FileStatus::kOk) return status;
  if (read != bytes.size() || bytes[0] != 1) return FileStatus::kIntegrity;
  for (size_t i = 1; i < 8; ++i) if (bytes[i]) return FileStatus::kIntegrity;
  uint64_t period = 0;
  for (size_t i = 8; i < 16; ++i) period = (period << 8) | bytes[i];
  if (period > static_cast<uint64_t>(INT64_MAX)) return FileStatus::kIntegrity;
  *policy = {static_cast<int64_t>(period), DecayTimebase::kSinceLastVisit};
  return FileStatus::kOk;
}
FileStatus WriteDecayPolicy(RecordFileSystem& files, DecayPolicy policy) {
  if (policy.period_us < 0 || policy.timebase != DecayTimebase::kSinceLastVisit)
    return FileStatus::kInvalid;
  std::array<uint8_t, 16> bytes{};
  bytes[0] = 1;
  auto period = static_cast<uint64_t>(policy.period_us);
  for (int i = 15; i >= 8; --i) { bytes[i] = period & 255; period >>= 8; }
  const SettingsReplacement replacement{files.ProfileCapability(kPurpose), kPurpose,
                                         kPolicyFile, bytes};
  return files.ReplaceSettings(base::span_from_ref(replacement));
}
FileStatus NextDecayedRecord(RecordFileSystem& files, int64_t now_us,
                            std::optional<RecordInfo>* record) {
  record->reset();
  DecayPolicy policy;
  auto status = ReadDecayPolicy(files, &policy);
  if (status != FileStatus::kOk || !policy.period_us) return status;
  status = files.ClampFutureRecordTimes(now_us);
  if (status != FileStatus::kOk) return status;
  std::vector<RecordInfo> records;
  status = files.ListRecords(&records);
  if (status != FileStatus::kOk) return status;
  for (auto& candidate : records) {
    const auto baseline = candidate.last_visit_us ? candidate.last_visit_us : candidate.created_us;
    if (baseline <= now_us && now_us - baseline >= policy.period_us) {
      *record = std::move(candidate); break;
    }
  }
  return FileStatus::kOk;
}
FileStatus DecayBeforeAdmission(std::shared_ptr<RecordFileSystem> files) {
  std::unique_ptr<content_settings::OwnedSettingsRegistries> registries;
  scoped_refptr<internal::SitePrefStore> prefs;
  for (;;) {
    std::optional<RecordInfo> expired;
    auto status = NextDecayedRecord(*files, DecayNowMicros(), &expired);
    if (status != FileStatus::kOk || !expired) return status;
    if (!prefs) {
      registries = std::make_unique<content_settings::OwnedSettingsRegistries>();
      prefs = base::MakeRefCounted<internal::SitePrefStore>(files,
          base::BindRepeating([](FileStatus) {}), &registries->website());
      prefs->ReadPrefs();
      if (prefs->last_status() != FileStatus::kOk) return prefs->last_status();
    }
    base::UnguessableToken ticket;
    status = prefs->RetireSite(ContentSettingsPattern::FromString(expired->domain).ToRepresentativeUrl(), &ticket,
                               &*expired);
    if (status == FileStatus::kBusy || status == FileStatus::kNotFound) continue;
    if (status != FileStatus::kOk) return status;
    RecordDeletion result;
    status = files->FinishRecordDeletion(ticket, &result);
    if (status != FileStatus::kOk) return status;
  }
}
}  // namespace alloy::site_records
