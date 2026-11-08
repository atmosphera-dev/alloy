// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_IMPL_H_
#define COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_IMPL_H_

#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/unguessable_token.h"
#include "components/services/app_service/public/cpp/preferred_apps_list.h"

namespace apps {
class AppServiceMojomImpl;
class AppServiceProxyPreferredAppsTest;

// Native matching/model owner. Persistence is injected by its composition root;
// neither this component nor its storage contract knows Profile or Alloy.
class PreferredAppsImpl : public PreferredAppsList::Delegate,
                          private PreferredAppsListHandle::Observer {
 public:
  enum class StorageResult { kOk, kDenied, kConflict, kIo, kInvalid };
  struct Version {
    base::UnguessableToken revision;
    base::UnguessableToken admission;
    bool operator==(const Version&) const = default;
  };
  struct Snapshot {
    Version version;
    PreferredApps entries;
    // Pure, owning-sequence check of this original generation, not a rebind.
    base::RepeatingCallback<bool()> admitted;
    // A successful read may need its canonical representation persisted (new
    // ordinary file or native format upgrade). Use the same versioned write
    // path and durable completion as a user edit, never a silent repair.
    bool requires_write_back = false;
  };
  class Storage {
   public:
    using Reply = base::OnceCallback<void(StorageResult, Snapshot)>;
    using Changed = base::RepeatingCallback<void(StorageResult)>;
    virtual ~Storage() = default;
    virtual void Read(Reply reply) = 0;
    virtual void Write(Version expected, PreferredApps entries, Reply reply) = 0;
    // Synchronous invalidation, before an obsolete cached grant is used again.
    // A successful own write is acknowledged through Reply, not echoed here.
    virtual void SetChangedCallback(Changed changed) = 0;
  };
  class Host {
   public:
    Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    ~Host() = default;
    virtual void OnSupportedLinksPreferenceChanged(const std::string& app_id, bool open_in_app) = 0;
    virtual bool QueryConflict(const std::string& first_app_id, const IntentFilterPtr& first_filter,
                               const std::string& second_app_id, const IntentFilterPtr& second_filter) = 0;
    virtual bool IsWebAppInExtendedScope(const GURL& url, const std::string& app_id) const = 0;
    virtual void OnPreferredAppsStorageError(StorageResult result) {}
  };
  PreferredAppsImpl(Host* host, const base::FilePath& profile_dir,
                    base::OnceClosure read_completed_for_testing = {},
                    base::OnceClosure write_completed_for_testing = {});
  PreferredAppsImpl(Host* host, std::shared_ptr<Storage> storage,
                    base::OnceClosure read_completed_for_testing = {},
                    base::OnceClosure write_completed_for_testing = {});
  PreferredAppsImpl(const PreferredAppsImpl&) = delete;
  PreferredAppsImpl& operator=(const PreferredAppsImpl&) = delete;
  ~PreferredAppsImpl() override;

  void RemovePreferredApp(const std::string& app_id);
  void SetSupportedLinksPreference(const std::string& app_id, IntentFilters all_link_filters);
  void RemoveSupportedLinksPreference(const std::string& app_id);
#if BUILDFLAG(IS_CHROMEOS)
  void SetProtocolLinkPreference(const std::string& app_id, IntentFilterPtr filter);
  void RemoveProtocolLinkFilters(const std::string& app_id, IntentFilters filters);
#endif
  bool QueryConflict(const std::string&, const IntentFilterPtr&,
                     const std::string&, const IntentFilterPtr&) override;
  bool IsWebAppInExtendedScope(const GURL& url, const std::string& app_id) const override;
  void SetLongestPrefixMatchEnabled(bool enabled);
  PreferredAppsListHandle& preferred_apps_list() { return preferred_apps_list_; }
  StorageResult storage_status() const { return status_; }

  enum class ChangeKind { kRemoveApp, kSetLinks, kRemoveLinks, kSetProtocol, kRemoveProtocol };
  struct Change {
    ChangeKind kind;
    std::string app_id;
    IntentFilters filters;
  };
  // Explicit retained user intent. Invalidated/failed work is never replayed
  // under fresh authority; the owner can present/reapply it as a new action.
  std::vector<PreferredApps> GetUnsavedValues() const;
  std::vector<Change> GetRejectedChanges() const;

 private:
  friend AppServiceMojomImpl;
  friend class AppServiceProxyPreferredAppsTest;
  struct Desired {
    uint64_t revision;
    PreferredApps entries;
  };
  bool Admitted() const;
  bool Current(base::UnguessableToken epoch, uint64_t revision) const;
  void ScheduleRead();
  void StartRead(base::UnguessableToken epoch, base::UnguessableToken ticket);
  void ReadCompleted(base::UnguessableToken epoch, base::UnguessableToken ticket,
                     StorageResult result, Snapshot snapshot);
  void StorageChanged(StorageResult result);
  void Invalidate(StorageResult result, bool refresh);
  void Submit(Change change);
  void Execute(Change change);
  void Reject(Change change);
  void PumpWrite();
  void WriteCompleted(base::UnguessableToken epoch, uint64_t revision,
                      StorageResult result, Snapshot snapshot);
  void OnPreferredAppChanged(const std::string& app_id, bool value) override;
  void OnPreferredAppsListInitialized() override {}
  void OnPreferredAppsListWillBeDestroyed(PreferredAppsListHandle*) override {}

  raw_ptr<Host> host_;
  PreferredAppsList preferred_apps_list_;
  const std::shared_ptr<Storage> storage_;
  std::optional<Snapshot> snapshot_;
  std::optional<Desired> desired_;
  std::deque<Change> pending_;
  std::vector<PreferredApps> unsaved_;
  std::vector<Change> rejected_;
  StorageResult status_ = StorageResult::kOk;
  base::UnguessableToken epoch_ = base::UnguessableToken::Create();
  base::UnguessableToken read_ticket_;
  uint64_t revision_ = 0;
  bool ready_ = false;
  bool writing_ = false;
  bool longest_prefix_match_enabled_ = false;
  base::OnceClosure read_completed_for_testing_;
  base::OnceClosure write_completed_for_testing_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<PreferredAppsImpl> weak_ptr_factory_{this};
};
}  // namespace apps
#endif  // COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_IMPL_H_
