// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_DAT_CACHE_MANAGER_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_DAT_CACHE_MANAGER_H_

#include <optional>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/components/brave_component_updater/browser/dat_file_util.h"
#include "brave/components/brave_shields/core/browser/ad_block_load_epoch.h"

using brave_component_updater::DATFileDataBuffer;

namespace brave_shields {

// A profile owner can supply its admitted storage without exposing paths or
// keys. The supplied backend owns error reporting; an authentication/I/O error
// must never invoke ReadReply with a fabricated pair of cache misses.
class AdBlockDATCacheManager {
 public:
  using ReadReply = base::OnceCallback<void(std::optional<DATFileDataBuffer>,
                                          std::optional<DATFileDataBuffer>)>;
  using SnapshotReply = base::OnceCallback<void(std::optional<DATFileDataBuffer>)>;
  struct Backend {
    base::RepeatingCallback<bool(ReadReply)> read;
    base::RepeatingCallback<bool(bool, DATFileDataBuffer,std::shared_ptr<AdBlockLoadEpoch>)> write;
    base::RepeatingCallback<bool()> drop;
    base::RepeatingCallback<bool(bool,std::string,SnapshotReply)> read_snapshot;
    base::RepeatingCallback<bool(bool,std::string,DATFileDataBuffer,std::shared_ptr<AdBlockLoadEpoch>)> write_snapshot;
  };
  explicit AdBlockDATCacheManager(const base::FilePath& profile_dir);
  explicit AdBlockDATCacheManager(Backend backend);
  ~AdBlockDATCacheManager();
  AdBlockDATCacheManager(const AdBlockDATCacheManager&) = delete;
  AdBlockDATCacheManager& operator=(const AdBlockDATCacheManager&) = delete;

  // False explicitly refuses scheduling. Accepted asynchronous storage errors
  // still go to the supplied backend's error owner, never to a cache miss.
  bool WriteDATFile(bool is_default_engine, DATFileDataBuffer dat,
      std::shared_ptr<AdBlockLoadEpoch> epoch = nullptr);
  bool DropDATCache();
  bool MaybeReadCachedDATFiles(ReadReply on_complete);
  bool ReadSnapshot(bool primary,std::string identity,SnapshotReply reply);
  bool WriteSnapshot(bool primary,std::string identity,DATFileDataBuffer dat,
      std::shared_ptr<AdBlockLoadEpoch> epoch);

 private:
  base::FilePath cache_dir_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::optional<Backend> backend_;
};

}  // namespace brave_shields
#endif
