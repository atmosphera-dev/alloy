// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/ad_block_dat_cache_manager.h"
#include "base/alloy/storage_policy.h"
#include "brave/alloy/bundled_data.h"

#include <optional>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "brave/components/brave_shields/core/common/features.h"

namespace brave_shields {
namespace {
constexpr char kAdblockCacheDir[] = "adblock_cache";
constexpr char kAdBlockEngine0DATCache[] = "engine0.dat";
constexpr char kAdBlockEngine1DATCache[] = "engine1.dat";
std::pair<std::optional<DATFileDataBuffer>, std::optional<DATFileDataBuffer>>
ReadCachedDATFilesFromDisk(base::FilePath cache_dir) {
  return std::make_pair(
      base::ReadFileToBytes(cache_dir.AppendASCII(kAdBlockEngine0DATCache)),
      base::ReadFileToBytes(cache_dir.AppendASCII(kAdBlockEngine1DATCache)));
}
}  // namespace

AdBlockDATCacheManager::AdBlockDATCacheManager(const base::FilePath& profile_dir)
    : cache_dir_(profile_dir.AppendASCII(kAdblockCacheDir)),
      task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}
AdBlockDATCacheManager::AdBlockDATCacheManager(Backend backend)
    : backend_(std::move(backend)) {
  CHECK(backend_->read && backend_->write && backend_->drop);
}
AdBlockDATCacheManager::~AdBlockDATCacheManager() = default;
bool AdBlockDATCacheManager::ReadSnapshot(bool primary,std::string identity,SnapshotReply reply) {
  CHECK(backend_&&backend_->read_snapshot&&identity.size()==64);
  return backend_->read_snapshot.Run(primary,std::move(identity),std::move(reply));
}
bool AdBlockDATCacheManager::WriteSnapshot(bool primary,std::string identity,
    DATFileDataBuffer dat,std::shared_ptr<AdBlockLoadEpoch> epoch) {
  CHECK(backend_&&backend_->write_snapshot&&identity.size()==64);
  return backend_->write_snapshot.Run(primary,std::move(identity),std::move(dat),std::move(epoch));
}

bool AdBlockDATCacheManager::WriteDATFile(bool is_default_engine,
                                       DATFileDataBuffer dat,std::shared_ptr<AdBlockLoadEpoch> epoch) {
  CHECK(base::FeatureList::IsEnabled(features::kAdblockDATCache));
  if (backend_) {
    return backend_->write.Run(is_default_engine, std::move(dat),std::move(epoch));
  }
  if (base::alloy::IsNoDiskProcess()) return false;
  return task_runner_->PostTask(FROM_HERE, base::BindOnce(
      [](DATFileDataBuffer dat, base::FilePath path,std::shared_ptr<AdBlockLoadEpoch> epoch) {
        auto write=[&] {
          if (!base::CreateDirectory(path.DirName())) return;
          base::ImportantFileWriter::WriteFileAtomically(path, base::as_string_view(dat));
        };
        if(epoch)epoch->Publish(write);else write();
      }, std::move(dat), cache_dir_.AppendASCII(
          is_default_engine ? kAdBlockEngine0DATCache : kAdBlockEngine1DATCache),std::move(epoch)));
}
bool AdBlockDATCacheManager::DropDATCache() {
  if (backend_) return backend_->drop.Run();
  if (base::alloy::IsNoDiskProcess()) return false;
  return task_runner_->PostTask(FROM_HERE, base::BindOnce(
      [](base::FilePath path) { base::DeletePathRecursively(path); }, cache_dir_));
}
bool AdBlockDATCacheManager::MaybeReadCachedDATFiles(ReadReply on_complete) {
  CHECK(base::FeatureList::IsEnabled(features::kAdblockDATCache));
  if (backend_) return backend_->read.Run(std::move(on_complete));
  if (base::alloy::IsNoDiskProcess() || !alloy::DataDirectory()) {
    return base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(std::move(on_complete), std::nullopt, std::nullopt));
  }
  return task_runner_->PostTaskAndReplyWithResult(FROM_HERE,
      base::BindOnce(&ReadCachedDATFilesFromDisk, cache_dir_),
      base::BindOnce([](ReadReply callback,
          std::pair<std::optional<DATFileDataBuffer>, std::optional<DATFileDataBuffer>> result) {
        std::move(callback).Run(std::move(result.first), std::move(result.second));
      }, std::move(on_complete)));
}
}  // namespace brave_shields
