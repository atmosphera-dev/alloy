// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_CAPABILITY_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_CAPABILITY_H_

#include <atomic>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/unguessable_token.h"
#include "brave/alloy/site_records/storage_mode.h"
#include "url/origin.h"

class GURL;

namespace alloy::site_records {

// A capability is bound to one store, not a general permission to use a record.
enum class StorePurpose {
  kCookies,
  kDomStorage,
  kIndexedDb,
  kCacheStorage,
  kFileSystem,
  kServiceWorkers,
  kBackgroundJobs,
  kHttpCache,
  kCodeCache,
  kHistory,
  kVisitedLinks,
  kFavicons,
  kSiteSettings,
  kQuota,
  kTransportSecurity,
  kServerProperties,
  kReportingConfiguration,
  kSharedDictionaries,
  kMediaDeviceSalts,
  kOriginTrials,
  kNotifications,
  kBlobSpill,
  kProfileSettings,
  kProfileCertificates,
  kProfileCustomFilters,
  kProfileDictionary,
  kProfileAssets,
  // Live document/network working state is distinct from storage admission.
  // Its owner must retire this generation at the relevant lifetime boundary.
  kDocumentWorkingState,
  kNetworkWorkingState,
};

enum class StorageAccess {
  kReadMemory,
  kWriteMemory,
  kReadRecordFiles,
  kWriteRecordFiles,
  kReadParentProfile,
  kRunBackgroundJob,
};

bool IsProfileStorePurpose(StorePurpose purpose);

// Browser-side ownership resolution supplies the validated document/hop URL
// and origin, not values claimed in a renderer message or a resource URL. The
// URL matters: about:blank can inherit an otherwise persistent origin. The
// result is an in-memory lookup key, NEVER a physical directory name or a
// security origin.
// Empty means no record (R16); origin/StorageKey security boundaries remain
// intact inside a record. SchemefulSite supplies private-registry/IP semantics.
std::string RecordDomainForPrincipal(const GURL& browser_owned_url,
                                     const url::Origin& browser_owned_origin);

class RecordCapabilityIssuer;

// Shared by every outstanding operation in one generation. Revocation is
// monotonic and visible across sequences; it cannot be undone by retaining a
// copy of a capability or by issuing another handle from a revoked issuer.
class RecordGeneration final
    : public base::RefCountedThreadSafe<RecordGeneration> {
 public:
  bool active() const { return active_.load(std::memory_order_acquire); }

 private:
  friend class base::RefCountedThreadSafe<RecordGeneration>;
  friend class RecordCapability;
  friend class RecordCapabilityIssuer;

  RecordGeneration(base::UnguessableToken profile,
                   base::UnguessableToken record,
                   base::UnguessableToken generation,
                   StorageMode mode);
  ~RecordGeneration();

  const base::UnguessableToken profile_;
  const base::UnguessableToken record_;
  const base::UnguessableToken generation_;
  const StorageMode mode_;
  std::atomic<bool> active_{true};
};

// Engine-only, immutable authority; no wire/deserialization constructor and no
// pathname, renderer ID or origin supplied by the caller of Allows(). Owners
// check admission BEFORE creating/opening/reading any backend, and recheck it
// before asynchronous writes/callbacks. Revocation alone is not a substitute
// for the owner's document/worker/handle teardown and write barrier.
class RecordCapability {
 public:
  RecordCapability();
  RecordCapability(const RecordCapability&);
  RecordCapability& operator=(const RecordCapability&);
  ~RecordCapability();

  bool Allows(StorePurpose expected_purpose, StorageAccess access) const;
  bool active() const;
  StorageMode mode() const;
  base::UnguessableToken profile() const;
  base::UnguessableToken record() const;
  base::UnguessableToken generation() const;

 private:
  friend class RecordCapabilityIssuer;
  RecordCapability(scoped_refptr<RecordGeneration> generation,
                   StorePurpose purpose);

  scoped_refptr<RecordGeneration> generation_;
  StorePurpose purpose_ = StorePurpose::kCookies;
};

// Owned by the browser-side profile/record/document lifecycle, never exposed
// through public Alloy/CEF APIs or renderer IPC. The resolver supplies opaque
// engine identities after determining frame/navigation/worker ownership. The
// encrypted catalogue will supply durable record/generation IDs; a document
// or embedded-memory owner uses its own transient IDs. No key or storage is
// created by this type. Destroying the owner revokes all outstanding handles.
class RecordCapabilityIssuer {
 public:
  RecordCapabilityIssuer(base::UnguessableToken profile,
                         base::UnguessableToken record,
                         base::UnguessableToken generation,
                         StorageMode mode);
  RecordCapabilityIssuer(const RecordCapabilityIssuer&) = delete;
  RecordCapabilityIssuer& operator=(const RecordCapabilityIssuer&) = delete;
  ~RecordCapabilityIssuer();

  RecordCapability Issue(StorePurpose purpose) const;
  void Revoke();

 private:
  scoped_refptr<RecordGeneration> generation_;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_CAPABILITY_H_
