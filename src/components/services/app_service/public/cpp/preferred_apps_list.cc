// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/services/app_service/public/cpp/preferred_apps_list.h"

#include <algorithm>
#include <utility>
#include "base/check.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "components/services/app_service/public/cpp/intent_util.h"
#include "url/gurl.h"

namespace apps {
PreferredAppsList::PreferredAppsList(Delegate* delegate) : delegate_(delegate) {}
PreferredAppsList::~PreferredAppsList() = default;
bool PreferredAppsList::Available() const { return available_ && (!admitted_ || admitted_.Run()); }
void PreferredAppsList::SetAvailabilityCallback(base::RepeatingCallback<bool()> admitted) {
  admitted_ = std::move(admitted);
  SetAvailable(available_);
}
PreferredApps PreferredAppsList::GetPendingValue() const { return ClonePreferredApps(preferred_apps_); }
base::flat_set<std::string> PreferredAppsList::Supported() const {
  base::flat_set<std::string> ids;
  if (available_) for (const auto& entry : preferred_apps_)
    if (apps_util::IsSupportedLinkForApp(entry->app_id, entry->intent_filter)) ids.insert(entry->app_id);
  return ids;
}
void PreferredAppsList::Notify(std::vector<std::pair<std::string, bool>> changes, bool initialized) {
  for (auto& [id, value] : changes)
    pending_notifications_.push_back({std::move(id), value, revision_});
  if (initialized) pending_initialization_ = true;
  if (notifying_) return;
  notifying_ = true;
  auto weak = weak_ptr_factory_.GetWeakPtr();
  // Drain on the outermost call, not recursively. A nested replacement must
  // invalidate stale positive events without losing removals owed to another
  // observer. Re-evaluate obsolete events from the current admitted model for
  // each consumer; fresh native operation events retain their native meaning.
  while (!pending_notifications_.empty() || pending_initialization_) {
    while (!pending_notifications_.empty()) {
      const auto change = std::move(pending_notifications_.front());
      pending_notifications_.pop_front();
      for (auto& observer : observers_) {
        const bool value = change.revision == revision_ ? change.value && Available()
            : IsPreferredAppForSupportedLinks(change.id);
        observer.OnPreferredAppChanged(change.id, value);
        if (!weak) return;
      }
    }
    if (pending_initialization_) {
      pending_initialization_ = false;
      for (auto& observer : observers_) {
        // Readiness belongs to the admitted model, not one filter revision.
        // A grant replacement must not strand another initialization consumer.
        if (!IsInitialized()) break;
        observer.OnPreferredAppsListInitialized();
        if (!weak) return;
      }
    }
  }
  notifying_ = false;
}
void PreferredAppsList::Init() { Init(PreferredApps()); }
void PreferredAppsList::Init(PreferredApps entries) {
  auto weak = weak_ptr_factory_.GetWeakPtr();
  Replace(std::move(entries));
  if (weak && IsInitialized()) Notify({}, true);
}
void PreferredAppsList::Replace(PreferredApps entries) {
  const auto old = Supported();
  auto previous = std::move(preferred_apps_);
  preferred_apps_ = std::move(entries);
  initialized_ = true;
  available_ = !admitted_ || admitted_.Run();
  ++revision_;
  const auto fresh = Supported();
  std::vector<std::pair<std::string, bool>> changes;
  for (const auto& id : old) if (!fresh.contains(id)) changes.emplace_back(id, false);
  for (const auto& id : fresh) {
    PreferredApps before, after;
    for (const auto& entry : previous) if (entry->app_id == id) before.push_back(entry->Clone());
    for (const auto& entry : preferred_apps_) if (entry->app_id == id) after.push_back(entry->Clone());
    if (!old.contains(id) || !apps::IsEqual(before, after)) changes.emplace_back(id, true);
  }
  Notify(std::move(changes), false);
}
void PreferredAppsList::SetAvailable(bool available) {
  const auto old = Supported();
  available_ = available && (!admitted_ || admitted_.Run());
  ++revision_;
  const auto fresh = Supported();
  std::vector<std::pair<std::string, bool>> changes;
  for (const auto& id : old) if (!fresh.contains(id)) changes.emplace_back(id, false);
  for (const auto& id : fresh) if (!old.contains(id)) changes.emplace_back(id, true);
  Notify(std::move(changes), false);
}

ReplacedAppPreferences PreferredAppsList::AddPreferredApp(const std::string& requested_app_id,
                                                          const IntentFilterPtr& requested_filter) {
  ReplacedAppPreferences replaced;
  if (!Available()) return replaced;
  // Inputs may refer into this model. The delegate can synchronously replace
  // or retire it, so both inputs and the candidate projection are owned here.
  const std::string app_id = requested_app_id;
  const auto filter = requested_filter->Clone();
  if (EntryExists(app_id, filter)) return replaced;
  auto weak = weak_ptr_factory_.GetWeakPtr();
  const auto revision = revision_;
  auto next = ClonePreferredApps(preferred_apps_);
  for (size_t i = 0; i < next.size();) {
    auto& entry = next[i];
    if (entry->app_id != app_id && apps_util::FiltersHaveOverlap(entry->intent_filter, filter)) {
      bool conflict = !delegate_ || delegate_->QueryConflict(entry->app_id, entry->intent_filter, app_id, filter);
      if (!weak || revision_ != revision || !Available()) return {};
      if (conflict) {
        replaced[entry->app_id].push_back(std::move(entry->intent_filter));
        next.erase(next.begin() + i);
        continue;
      }
    }
    ++i;
  }
  next.push_back(std::make_unique<PreferredApp>(filter->Clone(), app_id));
  preferred_apps_ = std::move(next);
  ++revision_;
  if (Available() && apps_util::IsSupportedLinkForApp(app_id, filter)) {
    std::vector<std::pair<std::string, bool>> changes{{app_id, true}};
    for (const auto& [id, filters] : replaced) changes.emplace_back(id, false);
    Notify(std::move(changes), false);
  }
  return replaced;
}
IntentFilters PreferredAppsList::DeletePreferredApp(const std::string& requested_app_id, const IntentFilterPtr& filter) {
  IntentFilters removed;
  if (!Available()) return removed;
  const std::string app_id = requested_app_id;
  // The caller may have passed the filter being removed by reference.
  const auto target = filter->Clone();
  for (auto it = preferred_apps_.begin(); it != preferred_apps_.end();) {
    if ((*it)->app_id == app_id && apps_util::FiltersHaveOverlap((*it)->intent_filter, target)) {
      removed.push_back(std::move((*it)->intent_filter)); it = preferred_apps_.erase(it);
    } else ++it;
  }
  ++revision_;
  if (Available() && apps_util::IsSupportedLinkForApp(app_id, target)) Notify({{app_id, false}}, false);
  return removed;
}
IntentFilters PreferredAppsList::DeleteAppId(const std::string& requested_app_id) {
  IntentFilters removed;
  if (!Available()) return removed;
  const std::string app_id = requested_app_id;
  for (auto it = preferred_apps_.begin(); it != preferred_apps_.end();) {
    if ((*it)->app_id == app_id) { removed.push_back(std::move((*it)->intent_filter)); it = preferred_apps_.erase(it); }
    else ++it;
  }
  ++revision_;
  if (Available()) Notify({{app_id, false}}, false);
  return removed;
}
IntentFilters PreferredAppsList::DeleteSupportedLinks(const std::string& requested_app_id) {
  IntentFilters removed;
  if (!Available()) return removed;
  const std::string app_id = requested_app_id;
  for (auto it = preferred_apps_.begin(); it != preferred_apps_.end();) {
    if ((*it)->app_id == app_id && apps_util::IsSupportedLinkForApp(app_id, (*it)->intent_filter)) {
      removed.push_back(std::move((*it)->intent_filter)); it = preferred_apps_.erase(it);
    } else ++it;
  }
  ++revision_;
  if (Available() && !removed.empty()) Notify({{app_id, false}}, false);
  return removed;
}
bool PreferredAppsList::IsInitialized() const { return initialized_ && Available(); }
size_t PreferredAppsList::GetEntrySize() const { return Available() ? preferred_apps_.size() : 0; }
PreferredApps PreferredAppsList::GetValue() const { return Available() ? ClonePreferredApps(preferred_apps_) : PreferredApps(); }
const PreferredApps& PreferredAppsList::GetReference() const { return Available() ? preferred_apps_ : unavailable_; }
bool PreferredAppsList::IsPreferredAppForSupportedLinks(const std::string& app_id) const {
  if (!Available()) return false;
  for (const auto& entry : preferred_apps_)
    if (entry->app_id == app_id && apps_util::IsSupportedLinkForApp(app_id, entry->intent_filter)) return true;
  return false;
}
std::optional<std::string> PreferredAppsList::FindPreferredAppForUrl(const GURL& url) const {
  return FindPreferredAppForIntent(std::make_unique<Intent>(apps_util::kIntentActionView, url));
}
std::optional<std::string> PreferredAppsList::FindPreferredAppForIntent(const IntentPtr& intent) const {
  DCHECK(intent);
  if (!Available()) return std::nullopt;
  int best_level = static_cast<int>(IntentFilterMatchLevel::kNone);
  size_t best_length = 0;
  std::optional<std::string> best;
  std::vector<const PreferredApp*> extended;
  auto weak = weak_ptr_factory_.GetWeakPtr();
  const auto revision = revision_;
  for (const auto& entry : preferred_apps_) {
    if (!intent->MatchFilter(entry->intent_filter)) continue;
    const std::string app_id = entry->app_id;
    const bool extension = delegate_ && intent->url && delegate_->IsWebAppInExtendedScope(*intent->url, app_id);
    if (!weak || revision_ != revision || !Available()) return std::nullopt;
    if (extension) { extended.push_back(entry.get()); continue; }
    const auto level = entry->intent_filter->GetFilterMatchLevel();
    const auto length = intent->url ? apps_util::IntentFilterUrlMatchLength(entry->intent_filter, *intent->url) : 0;
    if (level < best_level || (longest_prefix_match_enabled_ && level == best_level && length < best_length)) continue;
    best_level = level; best_length = length; best = entry->app_id;
  }
  if (best) return best;
  best_level = static_cast<int>(IntentFilterMatchLevel::kNone); best_length = 0;
  for (const auto* entry : extended) {
    const auto level = entry->intent_filter->GetFilterMatchLevel();
    const auto length = intent->url ? apps_util::IntentFilterUrlMatchLength(entry->intent_filter, *intent->url) : 0;
    if (level < best_level || (longest_prefix_match_enabled_ && level == best_level && length < best_length)) continue;
    best_level = level; best_length = length; best = entry->app_id;
  }
  return best;
}
base::flat_set<std::string> PreferredAppsList::FindPreferredAppsForFilters(std::optional<std::string> app_id,
                                                                       const IntentFilters& filters) const {
  base::flat_set<std::string> ids;
  if (!Available()) return ids;
  auto weak = weak_ptr_factory_.GetWeakPtr();
  const auto revision = revision_;
  const auto requested = CloneIntentFilters(filters);
  const auto entries = ClonePreferredApps(preferred_apps_);
  for (const auto& filter : requested) for (const auto& entry : entries) {
    if (app_id && entry->app_id == *app_id) continue;
    if (!apps_util::FiltersHaveOverlap(filter, entry->intent_filter)) continue;
    const bool conflict = !app_id || !delegate_ || delegate_->QueryConflict(entry->app_id, entry->intent_filter, *app_id, filter);
    if (!weak || revision_ != revision || !Available()) return {};
    if (conflict) ids.insert(entry->app_id);
  }
  return ids;
}
bool PreferredAppsList::EntryExists(const std::string& app_id, const IntentFilterPtr& filter) {
  for (const auto& entry : preferred_apps_) if (entry->app_id == app_id && *entry->intent_filter == *filter) return true;
  return false;
}
}  // namespace apps
