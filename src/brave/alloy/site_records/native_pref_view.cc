// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/native_pref_view.h"

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"

namespace alloy::site_records {
NativePrefSource::NativePrefSource() = default;
NativePrefSource::~NativePrefSource() = default;
scoped_refptr<NativePrefView> NativePrefSource::CreateNativeView() {
  if (!CanStartNativeConstruction()) return nullptr;
  const auto id = base::UnguessableToken::Create();
  auto view = base::WrapRefCounted(new NativePrefView(base::WrapRefCounted(this), id));
  native_views_.emplace(id, view->weak_factory_.GetWeakPtr());
  return view;
}
void NativePrefSource::CloseNativeViews() {
  const auto views = native_views_;
  for (const auto& [id, weak] : views) if (weak) weak->RequestTeardown();
}
void NativePrefSource::NativeViewClosed(base::UnguessableToken id) {
  native_views_.erase(id); NativeViewsChanged();
}

NativePrefView::NativePrefView(scoped_refptr<NativePrefSource> source, base::UnguessableToken id)
    : source_(std::move(source)), id_(id) {}
NativePrefView::~NativePrefView() {
  if (stage_ != Stage::kClosed) source_->NativeViewClosed(id_);
}
bool NativePrefView::BeginConstruction(base::OnceClosure teardown) {
  if (stage_ != Stage::kPrepared || !teardown || !source_->CanStartNativeConstruction()) return false;
  teardown_ = std::move(teardown); stage_ = Stage::kConstructing;
  return true;
}
void NativePrefView::ConstructionFinished() {
  if (stage_ != Stage::kConstructing) return;
  stage_ = Stage::kActive;
  if (teardown_requested_ || !CanStartServiceConstruction()) RequestTeardown();
}
void NativePrefView::RequestTeardown() {
  scoped_refptr<NativePrefView> alive(this);
  teardown_requested_ = true;
  if (stage_ == Stage::kPrepared) { FinishTeardown(); return; }
  if (stage_ == Stage::kConstructing || stage_ == Stage::kClosed || stage_ == Stage::kTearingDown) return;
  stage_ = Stage::kTearingDown;
  if (teardown_) std::move(teardown_).Run();
}
void NativePrefView::FinishTeardown() {
  if (stage_ == Stage::kClosed) return;
  scoped_refptr<NativePrefView> alive(this);
  stage_ = Stage::kClosed; teardown_.Reset();
  source_->NativeViewClosed(id_);
}
bool NativePrefView::CanStartServiceConstruction() const {
  return (stage_ == Stage::kConstructing || stage_ == Stage::kActive) &&
      !teardown_requested_ && source_->CanStartNativeConstruction();
}
bool NativePrefView::Live() const {
  return stage_ != Stage::kPrepared && stage_ != Stage::kClosed && source_->NativeCacheAlive();
}
bool NativePrefView::GetValue(std::string_view key, const base::Value** out) const {
  return Live() && source_->NativeGetValue(key, out);
}
base::DictValue NativePrefView::GetValues() const { return Live() ? source_->NativeGetValues() : base::DictValue(); }
bool NativePrefView::GetMutableValue(std::string_view key, base::Value** out) {
  return Live() && source_->NativeGetMutableValue(key, out);
}
void NativePrefView::SetValue(std::string_view key, base::Value value, uint32_t) {
  if (Live()) source_->NativeSetValue(key, std::move(value), true);
}
void NativePrefView::SetValueSilently(std::string_view key, base::Value value, uint32_t) {
  if (Live()) source_->NativeSetValue(key, std::move(value), false);
}
void NativePrefView::ReportValueChanged(std::string_view key, uint32_t) {
  if (Live()) source_->NativeReportValueChanged(key, true);
}
void NativePrefView::RemoveValue(std::string_view key, uint32_t) {
  if (Live()) source_->NativeRemoveValue(key);
}
void NativePrefView::RemoveValuesByPrefixSilently(std::string_view prefix) {
  if (Live()) source_->NativeRemovePrefix(prefix);
}
void NativePrefView::AddObserver(PrefStore::Observer* observer) { if (Live()) source_->AddObserver(observer); }
void NativePrefView::RemoveObserver(PrefStore::Observer* observer) { source_->RemoveObserver(observer); }
bool NativePrefView::HasObservers() const { return Live() && source_->HasObservers(); }
bool NativePrefView::IsInitializationComplete() const { return Live() && source_->IsInitializationComplete(); }
bool NativePrefView::ReadOnly() const { return !Live() || !source_->NativeCanPersist(); }
PersistentPrefStore::PrefReadError NativePrefView::GetReadError() const {
  return Live() ? PREF_READ_ERROR_NONE : PREF_READ_ERROR_ACCESS_DENIED;
}
PersistentPrefStore::PrefReadError NativePrefView::ReadPrefs() { return GetReadError(); }
void NativePrefView::ReadPrefsAsync(ReadErrorDelegate* delegate) {
  std::unique_ptr<ReadErrorDelegate> owned(delegate);
  const auto error = GetReadError();
  if (owned && error != PREF_READ_ERROR_NONE) owned->OnError(error);
}
void NativePrefView::CommitPendingWrite(base::OnceClosure reply, base::OnceClosure done) {
  source_->CommitPendingWrite(std::move(reply), std::move(done));
}
void NativePrefView::SchedulePendingLossyWrites() { source_->SchedulePendingLossyWrites(); }
void NativePrefView::OnStoreDeletionFromDisk() { source_->OnStoreDeletionFromDisk(); }
bool NativePrefView::HasReadErrorDelegate() const { return source_->HasReadErrorDelegate(); }
}  // namespace alloy::site_records
