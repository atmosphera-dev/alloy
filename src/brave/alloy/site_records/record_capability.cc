// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_capability.h"

#include <utility>

#include "base/check.h"
#include "net/base/alloy_network_record_target.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace alloy::site_records {
namespace {

bool IsKnownPurpose(StorePurpose purpose) {
  switch (purpose) {
    case StorePurpose::kCookies:
    case StorePurpose::kDomStorage:
    case StorePurpose::kIndexedDb:
    case StorePurpose::kCacheStorage:
    case StorePurpose::kFileSystem:
    case StorePurpose::kServiceWorkers:
    case StorePurpose::kBackgroundJobs:
    case StorePurpose::kHttpCache:
    case StorePurpose::kCodeCache:
    case StorePurpose::kHistory:
    case StorePurpose::kVisitedLinks:
    case StorePurpose::kFavicons:
    case StorePurpose::kSiteSettings:
    case StorePurpose::kQuota:
    case StorePurpose::kTransportSecurity:
    case StorePurpose::kServerProperties:
    case StorePurpose::kReportingConfiguration:
    case StorePurpose::kSharedDictionaries:
    case StorePurpose::kMediaDeviceSalts:
    case StorePurpose::kOriginTrials:
    case StorePurpose::kNotifications:
    case StorePurpose::kBlobSpill:
    case StorePurpose::kProfileSettings:
    case StorePurpose::kProfileCertificates:
    case StorePurpose::kProfileCustomFilters:
    case StorePurpose::kProfileDictionary:
    case StorePurpose::kProfileAssets:
    case StorePurpose::kDocumentWorkingState:
    case StorePurpose::kNetworkWorkingState:
      return true;
  }
  return false;
}

bool IsWorkingState(StorePurpose purpose) {
  return purpose == StorePurpose::kDocumentWorkingState ||
         purpose == StorePurpose::kNetworkWorkingState;
}

}  // namespace

bool IsProfileStorePurpose(StorePurpose purpose) {
  return purpose == StorePurpose::kProfileSettings ||
         purpose == StorePurpose::kProfileCertificates ||
         purpose == StorePurpose::kProfileCustomFilters ||
         purpose == StorePurpose::kProfileDictionary ||
         purpose == StorePurpose::kProfileAssets;
}

std::string RecordDomainForPrincipal(const GURL& principal_url,
                                     const url::Origin& origin) {
  return net::AlloyRecordDomainForPrincipal(principal_url, origin);
}

RecordGeneration::RecordGeneration(base::UnguessableToken profile,
                                   base::UnguessableToken record,
                                   base::UnguessableToken generation,
                                   StorageMode mode,
                                   scoped_refptr<RecordGeneration> parent)
    : profile_(profile), record_(record), generation_(generation), mode_(mode),
      parent_(std::move(parent)) {
  CHECK(!profile_.is_empty());
  CHECK(!record_.is_empty());
  CHECK(!generation_.is_empty());
}

RecordGeneration::~RecordGeneration() = default;

RecordCapability::RecordCapability() = default;
RecordCapability::RecordCapability(const RecordCapability&) = default;
RecordCapability& RecordCapability::operator=(const RecordCapability&) =
    default;
RecordCapability::~RecordCapability() = default;

RecordCapability::RecordCapability(scoped_refptr<RecordGeneration> generation,
                                   StorePurpose purpose)
    : generation_(std::move(generation)), purpose_(purpose) {}

bool RecordCapability::Allows(StorePurpose purpose,
                              StorageAccess access) const {
  if (!active() || purpose != purpose_ || !IsKnownPurpose(purpose)) {
    return false;
  }
  switch (access) {
    case StorageAccess::kReadMemory:
    case StorageAccess::kWriteMemory:
      switch (mode()) {
        case StorageMode::kStateless:
          return IsWorkingState(purpose);
        case StorageMode::kMemory:
        case StorageMode::kEncrypted:
          return true;
      }
      return false;
    case StorageAccess::kReadRecordFiles:
    case StorageAccess::kWriteRecordFiles:
      return mode() == StorageMode::kEncrypted && !IsWorkingState(purpose);
    case StorageAccess::kReadParentProfile:
      return false;
    case StorageAccess::kRunBackgroundJob:
      return (mode() == StorageMode::kMemory ||
              mode() == StorageMode::kEncrypted) &&
             (purpose == StorePurpose::kServiceWorkers ||
              purpose == StorePurpose::kBackgroundJobs);
  }
  return false;
}

bool RecordCapability::active() const {
  return generation_ && generation_->active();
}

StorageMode RecordCapability::mode() const {
  return generation_ ? generation_->mode_ : StorageMode::kStateless;
}

base::UnguessableToken RecordCapability::profile() const {
  return generation_ ? generation_->profile_ : base::UnguessableToken();
}

base::UnguessableToken RecordCapability::record() const {
  return generation_ ? generation_->record_ : base::UnguessableToken();
}

base::UnguessableToken RecordCapability::generation() const {
  return generation_ ? generation_->generation_ : base::UnguessableToken();
}

RecordCapabilityIssuer::RecordCapabilityIssuer(
    base::UnguessableToken profile,
    base::UnguessableToken record,
    base::UnguessableToken generation,
    StorageMode mode)
    : generation_(new RecordGeneration(profile, record, generation, mode)) {}

RecordCapabilityIssuer::~RecordCapabilityIssuer() {
  Revoke();
}

RecordCapabilityIssuer::RecordCapabilityIssuer(
    const RecordCapability& profile_settings, base::UnguessableToken record,
    base::UnguessableToken generation, StorageMode mode)
    : generation_(new RecordGeneration(profile_settings.profile(), record,
          generation, mode, profile_settings.generation_)) {
  CHECK(profile_settings.purpose_ == StorePurpose::kProfileSettings);
  CHECK(profile_settings.mode() == StorageMode::kEncrypted);
}

RecordCapability RecordCapabilityIssuer::Issue(StorePurpose purpose) const {
  return RecordCapability(generation_, purpose);
}

void RecordCapabilityIssuer::Revoke() {
  generation_->active_.store(false, std::memory_order_release);
}

}  // namespace alloy::site_records
