// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVICE_API_MANAGED_CONFIGURATION_STORE_H_
#define CHROME_BROWSER_DEVICE_API_MANAGED_CONFIGURATION_STORE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "base/task/sequenced_task_runner.h"
#include "base/callback_list.h"
#include "components/value_store/value_store.h"
#include "url/origin.h"

// Blocking, sequence-owned native cache. Configuration and its version share
// one ValueStore snapshot, never a separately acknowledged profile preference.
class ManagedConfigurationStore {
 public:
  struct Options {
    base::RepeatingCallback<std::unique_ptr<value_store::ValueStore>()> create;
    base::RepeatingCallback<bool()> admitted;
    base::RepeatingCallback<void(const value_store::ValueStore::Status&)> error;
    scoped_refptr<base::SequencedTaskRunner> runner;
    base::RepeatingCallback<base::CallbackListSubscription(base::RepeatingClosure)> observe_retirement;
  };
  struct Version {
    bool success = false;
    std::optional<std::string> hash;
  };
  struct UpdateResult {
    bool success = false;
    bool changed = false;
  };

  ManagedConfigurationStore(const url::Origin& origin, Options options);
  ~ManagedConfigurationStore();
  ManagedConfigurationStore(const ManagedConfigurationStore&) = delete;
  ManagedConfigurationStore& operator=(const ManagedConfigurationStore&) = delete;

  Version GetVersion();
  UpdateResult SetCurrentPolicy(std::string hash, base::DictValue configuration);
  UpdateResult Clear();
  std::optional<base::DictValue> Get(const std::vector<std::string>& keys);

 private:
  struct Snapshot {
    std::optional<std::string> hash;
    base::DictValue configuration;
  };
  std::optional<Snapshot> Read();
  bool Ready();
  void Error(const value_store::ValueStore::Status& status);
  const url::Origin origin_;
  const Options options_;
  std::unique_ptr<value_store::ValueStore> store_;
  SEQUENCE_CHECKER(sequence_checker_);
};

#endif  // CHROME_BROWSER_DEVICE_API_MANAGED_CONFIGURATION_STORE_H_
