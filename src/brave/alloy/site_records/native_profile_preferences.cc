// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/native_profile_preferences.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "brave/alloy/site_records/pref_cache_clear.h"
namespace alloy::site_records {
NativeProfilePreferences::NativeProfilePreferences(RecordCapability original,
    scoped_refptr<base::SequencedTaskRunner> runner, base::RepeatingCallback<bool()> admitted,
    base::RepeatingClosure error)
    : original_(std::move(original)), runner_(std::move(runner)),
      admitted_(std::move(admitted)), error_(std::move(error)), cache_(base::MakeRefCounted<InMemoryPrefStore>()) {
  CHECK(runner_ && admitted_ && error_); CHECK(original_.mode()==StorageMode::kEncrypted);
}
NativeProfilePreferences::~NativeProfilePreferences() { Wipe(); }
bool NativeProfilePreferences::CanStartNativeConstruction() const {
  return !closing_ && admitted_.Run() && original_.Allows(
      StorePurpose::kProfileSettings,StorageAccess::kReadRecordFiles);
}
bool NativeProfilePreferences::NativeCacheAlive() const { return !cache_closed_; }
bool NativeProfilePreferences::NativeCanPersist() const { return CanStartNativeConstruction(); }
bool NativeProfilePreferences::GetValue(std::string_view key,const base::Value** out) const {return !ReadOnly()&&cache_->GetValue(key,out);}
base::DictValue NativeProfilePreferences::GetValues() const {return !ReadOnly()?cache_->GetValues():base::DictValue();}
bool NativeProfilePreferences::GetMutableValue(std::string_view key,base::Value** out) {return !ReadOnly()&&cache_->GetMutableValue(key,out);}
void NativeProfilePreferences::SetValue(std::string_view key,base::Value value,uint32_t flags) {if(!ReadOnly())cache_->SetValue(key,std::move(value),flags);}
void NativeProfilePreferences::SetValueSilently(std::string_view key,base::Value value,uint32_t flags) {if(!ReadOnly())cache_->SetValueSilently(key,std::move(value),flags);}
void NativeProfilePreferences::ReportValueChanged(std::string_view key,uint32_t flags) {if(!ReadOnly())cache_->ReportValueChanged(key,flags);}
void NativeProfilePreferences::RemoveValue(std::string_view key,uint32_t flags) {if(!ReadOnly())cache_->RemoveValue(key,flags);}
void NativeProfilePreferences::RemoveValuesByPrefixSilently(std::string_view prefix) {if(!ReadOnly())cache_->RemoveValuesByPrefixSilently(prefix);}
void NativeProfilePreferences::AddObserver(PrefStore::Observer* observer) {cache_->AddObserver(observer);}
void NativeProfilePreferences::RemoveObserver(PrefStore::Observer* observer) {cache_->RemoveObserver(observer);}
bool NativeProfilePreferences::HasObservers() const {return cache_->HasObservers();}
bool NativeProfilePreferences::IsInitializationComplete() const {return !closed_;}
bool NativeProfilePreferences::ReadOnly() const {return !CanStartNativeConstruction();}
PersistentPrefStore::PrefReadError NativeProfilePreferences::GetReadError() const {return closed_?PREF_READ_ERROR_ACCESS_DENIED:PREF_READ_ERROR_NONE;}
PersistentPrefStore::PrefReadError NativeProfilePreferences::ReadPrefs() {return GetReadError();}
void NativeProfilePreferences::ReadPrefsAsync(ReadErrorDelegate* delegate) {
  std::unique_ptr<ReadErrorDelegate> owned(delegate);if(owned&&closed_)owned->OnError(GetReadError());
}
void NativeProfilePreferences::CommitPendingWrite(base::OnceClosure reply,base::OnceClosure done) {
  // There are no files in this input, but native shutdown callers still need
  // their synchronous-done callback on the original worker FIFO, not on UI.
  if(!done)done=base::DoNothing();
  if(!reply)reply=base::DoNothing();
  const bool posted=runner_->PostTaskAndReply(FROM_HERE,std::move(done),std::move(reply));
  if(!posted)error_.Run();
}
void NativeProfilePreferences::SchedulePendingLossyWrites() {}
void NativeProfilePreferences::OnStoreDeletionFromDisk() {Close({});}
bool NativeProfilePreferences::HasReadErrorDelegate() const {return false;}
bool NativeProfilePreferences::NativeGetValue(std::string_view key,const base::Value** out) const {return !closed_&&cache_->GetValue(key,out);}
base::DictValue NativeProfilePreferences::NativeGetValues() const {return !closed_?cache_->GetValues():base::DictValue();}
bool NativeProfilePreferences::NativeGetMutableValue(std::string_view key,base::Value** out) {return !closed_&&cache_->GetMutableValue(key,out);}
void NativeProfilePreferences::NativeSetValue(std::string_view key,base::Value value,bool notify) {
  if(closed_)return;
  if(notify)cache_->SetValue(key,std::move(value),0);else cache_->SetValueSilently(key,std::move(value),0);
}
void NativeProfilePreferences::NativeReportValueChanged(std::string_view key,bool notify) {if(!closed_&&notify)cache_->ReportValueChanged(key,0);}
void NativeProfilePreferences::NativeRemoveValue(std::string_view key) {if(!closed_)cache_->RemoveValue(key,0);}
void NativeProfilePreferences::NativeRemovePrefix(std::string_view prefix) {if(!closed_)cache_->RemoveValuesByPrefixSilently(prefix);}
void NativeProfilePreferences::NativeViewsChanged() {MaybeClose();}
void NativeProfilePreferences::Wipe() {
  auto values=cache_->GetValues();
  for(auto entry:values){base::Value* stored=nullptr;if(cache_->GetMutableValue(entry.first,&stored)){base::DictValue moved;moved.Set("value",std::move(*stored));ClearPrefCache(moved);}}
  ClearPrefCache(values);cache_->RemoveValuesByPrefixSilently("");
}
void NativeProfilePreferences::Close(base::OnceClosure done) {
  scoped_refptr<NativeProfilePreferences> alive(this);
  if(closed_){if(done)std::move(done).Run();return;}
  if(done)closed_callbacks_.push_back(std::move(done));
  if(!closing_){closing_=true;CloseNativeViews();}
  MaybeClose();
}
void NativeProfilePreferences::MaybeClose() {
  if(!closing_||cache_closed_||!NativeViewsEmpty())return;
  scoped_refptr<NativeProfilePreferences> alive(this);cache_closed_=true;Wipe();
  // Finish in a later UI turn, after the native destructor has unwound and
  // the original FIFO has released all preceding shutdown callbacks.
  const bool posted=runner_->PostTaskAndReply(FROM_HERE,base::DoNothing(),
      base::BindOnce(&NativeProfilePreferences::FinishClose,alive));
  if(!posted){error_.Run();FinishClose();}
}
void NativeProfilePreferences::FinishClose() {
  scoped_refptr<NativeProfilePreferences> alive(this);closed_=true;
  auto callbacks=std::move(closed_callbacks_);for(auto& callback:callbacks)std::move(callback).Run();
}
}  // namespace alloy::site_records
