// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_CUSTOM_RESOURCE_PROVIDER_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_CUSTOM_RESOURCE_PROVIDER_H_
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/values.h"
#include "brave/components/brave_shields/core/browser/ad_block_resource_provider.h"
namespace value_store { class ValueStoreFrontend; }
class PrefService;
namespace brave_shields {
class AdBlockCustomResourceProvider : public AdBlockResourceProvider,
                                      private AdBlockResourceProvider::Observer {
 public:
  enum class ErrorCode { kOk, kInvalid, kAlreadyExists, kNotFound, kReadOnly, kStorage, kCancelled };
  struct Observer : public base::CheckedObserver {
    ~Observer() override = default;
    virtual void OnCustomResourcesChanged() {}
  };
  using GetCallback = base::OnceCallback<void(base::Value)>;
  using StatusCallback = base::OnceCallback<void(ErrorCode)>;
  using ReadCallback = base::OnceCallback<void(ErrorCode, base::Value)>;
  struct Backend {
    base::RepeatingCallback<bool()> available;
    base::RepeatingCallback<bool()> can_edit;
    base::RepeatingCallback<void(ReadCallback)> read;
    base::RepeatingCallback<void(base::Value, StatusCallback)> write;
    base::RepeatingCallback<base::CallbackListSubscription(base::RepeatingClosure)> observe_close;
  };
  AdBlockCustomResourceProvider(const base::FilePath& storage_root,
      std::unique_ptr<AdBlockResourceProvider> default_provider);
  AdBlockCustomResourceProvider(Backend backend,
      std::unique_ptr<AdBlockResourceProvider> default_provider);
  ~AdBlockCustomResourceProvider() override;
  void GetCustomResources(GetCallback callback);
  void GetCustomResourcesWithStatus(ReadCallback callback);
  void AddResource(PrefService*, const base::Value&, StatusCallback);
  void UpdateResource(PrefService*, const std::string&, const base::Value&, StatusCallback);
  void RemoveResource(PrefService*, const std::string&, StatusCallback);
  void AddObserver(Observer*);
  void RemoveObserver(Observer*);
  void LoadResources(base::OnceCallback<void(AdblockResourceStorageBox)>) override;
  void OverrideResourcesForTesting(AdblockResourceStorageBox);
  void Close();
 private:
  enum class Kind { kAdd, kUpdate, kRemove };
  struct Mutation { Kind kind; std::string name; base::Value resource; StatusCallback done; };
  bool Available() const;
  bool ValidateEdit(StatusCallback*);
  void Enqueue(Mutation);
  void Pump();
  void Mutate(ErrorCode, base::Value);
  void Saved(ErrorCode);
  void Completed(ErrorCode);
  void OnResourcesLoaded(AdblockResourceStorageBox) override;
  void OnDefaultResourcesLoaded(uint64_t,base::OnceCallback<void(AdblockResourceStorageBox)>, AdblockResourceStorageBox);
  void OnCustomResourcesLoaded(uint64_t,base::OnceCallback<void(AdblockResourceStorageBox)>,
      AdblockResourceStorageBox, ErrorCode, base::Value);
  void ReloadResourcesAndNotify();
  std::unique_ptr<AdBlockResourceProvider> default_resource_provider_;
  std::unique_ptr<value_store::ValueStoreFrontend> storage_;
  std::optional<Backend> backend_;
  base::CallbackListSubscription retirement_;
  base::ObserverList<Observer> observers_;
  std::optional<AdblockResourceStorageBox> cached_storage_;
  std::deque<Mutation> pending_;
  std::optional<Mutation> current_;
  bool closed_ = false;
  uint64_t revision_ = 0;
  base::WeakPtrFactory<AdBlockCustomResourceProvider> weak_ptr_factory_{this};
};
}  // namespace brave_shields
#endif
