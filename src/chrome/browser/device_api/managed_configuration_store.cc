// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/device_api/managed_configuration_store.h"

#include <utility>

#include "base/check.h"

namespace {
constexpr char kSnapshot[] = "managed-configuration-snapshot-v1";
using Store = value_store::ValueStore;
}  // namespace

ManagedConfigurationStore::ManagedConfigurationStore(const url::Origin& origin,
                                                     Options options)
    : origin_(origin), options_(std::move(options)) {
  CHECK(options_.create && options_.admitted && options_.error);
}
ManagedConfigurationStore::~ManagedConfigurationStore() = default;

void ManagedConfigurationStore::Error(const Store::Status& status) {
  auto callback = options_.error;
  callback.Run(status);  // Last external action: callback may retire its owner.
}

bool ManagedConfigurationStore::Ready() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!options_.admitted.Run()) {
    store_.reset();
    Error(Store::Status(Store::READ_ONLY, "Managed configuration unavailable"));
    return false;
  }
  if (!store_) store_ = options_.create.Run();
  if (!store_) {
    Error(Store::Status(Store::READ_ONLY, "Managed configuration backend unavailable"));
    return false;
  }
  return true;
}

std::optional<ManagedConfigurationStore::Snapshot> ManagedConfigurationStore::Read() {
  if (!Ready()) return std::nullopt;
  auto result = store_->Get();
  if (!result.status().ok()) {
    Error(result.status());
    return std::nullopt;
  }
  if (result.settings().empty()) return Snapshot();
  const auto* value = result.settings().FindDict(kSnapshot);
  if (!value || value->size() != 4 || result.settings().size() != 1 || value->FindInt("version") != 1 ||
      !value->FindString("origin") || *value->FindString("origin") != origin_.Serialize() ||
      !value->FindString("hash") || !value->FindDict("configuration")) {
    Error(Store::Status(Store::CORRUPTION, "Invalid managed configuration snapshot"));
    return std::nullopt;
  }
  const auto& configuration = *value->FindDict("configuration");
  for (auto [key, entry] : configuration) {
    if (!entry.is_string()) {
      Error(Store::Status(Store::CORRUPTION, "Invalid managed configuration value"));
      return std::nullopt;
    }
  }
  Snapshot snapshot;
  snapshot.hash = *value->FindString("hash");
  snapshot.configuration = configuration.Clone();
  return snapshot;
}

ManagedConfigurationStore::Version ManagedConfigurationStore::GetVersion() {
  auto snapshot = Read();
  if (!snapshot) return {};
  return {true, std::move(snapshot->hash)};
}

ManagedConfigurationStore::UpdateResult ManagedConfigurationStore::SetCurrentPolicy(
    std::string hash, base::DictValue configuration) {
  auto previous = Read();
  if (!previous) return {};
  for (auto [key, value] : configuration) {
    if (!value.is_string()) {
      Error(Store::Status(Store::OTHER_ERROR, "Managed configuration must contain serialized values"));
      return {};
    }
  }
  const bool changed = previous->configuration != configuration;
  if (previous->hash == hash && !changed) return {true, false};
  base::DictValue snapshot;
  snapshot.Set("version", 1);
  snapshot.Set("origin", origin_.Serialize());
  snapshot.Set("hash", std::move(hash));
  snapshot.Set("configuration", std::move(configuration));
  if (!options_.admitted.Run()) {
    Error(Store::Status(Store::READ_ONLY, "Managed configuration unavailable"));
    return {};
  }
  auto result = store_->Set(Store::IGNORE_QUOTA, kSnapshot, base::Value(std::move(snapshot)));
  if (!result.status().ok()) {
    Error(result.status());
    return {};
  }
  return {true, changed};
}

ManagedConfigurationStore::UpdateResult ManagedConfigurationStore::Clear() {
  auto previous = Read();
  if (!previous) return {};
  if (!previous->hash) return {true, false};
  auto result = store_->Remove(kSnapshot);
  if (!result.status().ok()) {
    Error(result.status());
    return {};
  }
  return {true, !previous->configuration.empty()};
}

std::optional<base::DictValue> ManagedConfigurationStore::Get(
    const std::vector<std::string>& keys) {
  auto snapshot = Read();
  if (!snapshot) return std::nullopt;
  base::DictValue result;
  for (const auto& key : keys) {
    if (const auto* value = snapshot->configuration.Find(key)) result.Set(key, value->Clone());
  }
  return result;
}
