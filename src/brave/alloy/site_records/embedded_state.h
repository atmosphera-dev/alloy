// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_EMBEDDED_STATE_H_
#define BRAVE_ALLOY_SITE_RECORDS_EMBEDDED_STATE_H_

#include <compare>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "brave/alloy/site_records/record_capability.h"
#include "components/keyed_service/core/keyed_service.h"
#include "url/gurl.h"

namespace alloy::site_records {

enum class EmbeddedStateScope { kPerOuterSite, kPerTab };

// Native profile settings implement this read-only interface. This is the only
// exception to temporary embedded backing; cookie grants, entity lists and
// Shields settings are not inputs. The predicate has SitePrefStore's native
// owning-UI-sequence signature; that projection owns R16 canonicalization. The
// consumer passes trusted browser URLs, never another aggregate domain list.
class EmbeddedFirstPartyPolicy {
 public:
  using Predicate =
      base::RepeatingCallback<bool(const GURL& outer, const GURL& embedded)>;
  // The binding captures the explicit owning projection, e.g. BindRepeating(
  // &SitePrefStore::IsConsideredFirstParty, owning_projection). No lookup or
  // filesystem work happens here; lost original admission returns false there.
  static std::unique_ptr<EmbeddedFirstPartyPolicy> Bind(Predicate predicate);
  virtual ~EmbeddedFirstPartyPolicy() = default;
  virtual bool IsConsideredFirstParty(const GURL& outer,
                                      const GURL& embedded) const = 0;
};

// Backing instances belong to the scope, not its borrowers. Retaining a worker,
// callback or handle cannot extend admission or retain a live native backend
// after retirement. Native adapters keep their own origin/StorageKey checks.
class EmbeddedState final : public base::RefCounted<EmbeddedState> {
 public:
  using ServiceFactory =
      base::OnceCallback<std::unique_ptr<KeyedService>(RecordCapability)>;

  EmbeddedState(base::UnguessableToken profile);
  RecordCapability Capability(StorePurpose purpose) const;
  // Native instance identities (e.g. a partition) select distinct reserved
  // factory slots. Complete callback-bearing initialization inside the factory,
  // so retirement during construction cannot publish or destroy a live caller.
  KeyedService* GetOrCreateService(const void* type,
                                   StorePurpose purpose,
                                   ServiceFactory factory,
                                   const void* native_instance = nullptr);
  // A logical scope/site revocation closes all affected issuers before any
  // native shutdown callback. Retire then destroys the detached backends.
  void RevokeAdmission();
  void Retire();

 private:
  friend class base::RefCounted<EmbeddedState>;
  ~EmbeddedState();
  struct ServiceEntry {
    StorePurpose purpose;
    std::unique_ptr<KeyedService> service;
  };
  RecordCapabilityIssuer issuer_;
  using ServiceKey = std::pair<const void*, const void*>;
  struct ServiceKeyLess {
    bool operator()(const ServiceKey& a, const ServiceKey& b) const {
      const std::less<const void*> less;
      return less(a.first, b.first) ||
             (a.first == b.first && less(a.second, b.second));
    }
  };
  std::map<ServiceKey, ServiceEntry, ServiceKeyLess> services_;
  SEQUENCE_CHECKER(sequence_checker_);
};

// One registry per native request context. The content adapter supplies opaque
// tab identities and trusted committed outer principals. Same-site reloads keep
// their scope; moving away or closing retires per-tab backing immediately. The
// per-outer-site variant shares backing while any tab of that outer site lives,
// then keeps it for Brave's 30 seconds. No StoragePartition swap is involved.
class EmbeddedStateRegistry final {
 public:
  enum class Backing { kNone, kRealSite, kTemporary };
  struct Resolution {
    Backing backing = Backing::kNone;
    // Present only for temporary backing. kRealSite is a routing decision, not
    // authority to open a record: that record's owner must resolve its own key.
    scoped_refptr<EmbeddedState> temporary;
    bool considered_first_party = false;
    // Distinguish revoked captured temporary authority from a principal for
    // which embedded backing was never applicable. Neither grants authority.
    bool temporary_admission_closed = false;
  };

  EmbeddedStateRegistry(base::UnguessableToken profile,
                        StorageMode mode,
                        EmbeddedStateScope scope,
                        std::unique_ptr<EmbeddedFirstPartyPolicy> policy);
  ~EmbeddedStateRegistry();
  EmbeddedStateRegistry(const EmbeddedStateRegistry&) = delete;
  EmbeddedStateRegistry& operator=(const EmbeddedStateRegistry&) = delete;

  void CommitOuterSite(base::UnguessableToken tab,
                       const GURL& document_url,
                       const url::Origin& document_origin);
  void CloseTab(base::UnguessableToken tab);
  Resolution Resolve(base::UnguessableToken tab,
                     const GURL& frame_url,
                     const url::Origin& frame_origin);
  // Engine deletion/decay uses a trusted principal, canonicalized with R16.
  // Revoke this site in every outer/tab scope; deleting an outer also retires
  // its whole jar and tab admission. Siblings in other jars remain untouched.
  void RevokeSite(const GURL& document_url, const url::Origin& document_origin);
  void Shutdown();

 private:
  struct ScopeKey {
    std::string outer_site;
    base::UnguessableToken tab;
    friend auto operator<=>(const ScopeKey&, const ScopeKey&) = default;
  };
  struct Entry {
    std::map<std::string, scoped_refptr<EmbeddedState>> sites;
    base::OneShotTimer keepalive;
  };
  struct PendingCommit {
    base::UnguessableToken operation;
    std::string site;
  };
  struct TabOwner {
    std::string site;
    base::UnguessableToken generation;
    GURL url;
  };
  ScopeKey Key(base::UnguessableToken tab, const std::string& outer_site) const;
  void RemoveTab(base::UnguessableToken tab);
  void Retire(const ScopeKey& key);

  const base::UnguessableToken profile_;
  const StorageMode mode_;
  const EmbeddedStateScope scope_;
  const std::unique_ptr<EmbeddedFirstPartyPolicy> policy_;
  std::map<base::UnguessableToken, TabOwner> tabs_;
  std::map<base::UnguessableToken, PendingCommit> pending_commits_;
  // Per-operation reservations match pending_commits_: synchronous callbacks
  // cannot complete an admission that site deletion already canceled.
  std::map<base::UnguessableToken, std::pair<std::string, std::string>>
      pending_resolutions_;
  std::set<std::string> revoking_sites_;
  std::map<ScopeKey, std::unique_ptr<Entry>> scopes_;
  bool closed_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<EmbeddedStateRegistry> weak_factory_{this};
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_EMBEDDED_STATE_H_
