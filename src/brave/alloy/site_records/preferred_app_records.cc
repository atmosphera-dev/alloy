// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/preferred_app_records.h"

#include <limits>
#include <map>
#include <optional>
#include <set>

#include "base/unguessable_token.h"
#include "brave/alloy/site_records/record_capability.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "components/services/app_service/public/cpp/intent_util.h"

namespace alloy::site_records {
namespace {
bool Authority(const apps::Condition& condition) {
  return condition.condition_type == apps::ConditionType::kAuthority;
}

std::string AuthorityOwner(const apps::ConditionValue& value) {
  auto host = apps_util::AuthorityView::Decode(value.value).host;
  if (value.match_type == apps::PatternMatchType::kSuffix && host.starts_with('.')) {
    host.remove_prefix(1);
  } else if (value.match_type != apps::PatternMatchType::kLiteral) {
    // A selector spanning unrelated sites is explicit global dispatch policy,
    // not a fact about any particular visited origin.
    return {};
  }
  const GURL url("https://" + std::string(host) + "/");
  if (!url.is_valid() || url.has_username() || url.has_password() ||
      url.GetPath() != "/" || url.has_query() || url.has_ref()) return {};
  return RecordDomainForPrincipal(url, url::Origin::Create(url));
}

bool ValidFilter(const base::DictValue& filter) {
  for (auto [key, value] : filter) {
    if (key == apps_util::kActivityNameKey || key == apps_util::kActivityLabelKey) {
      if (!value.is_string()) return false;
      continue;
    }
    if (key != apps_util::kConditionsKey || !value.is_list()) return false;
    for (const auto& entry : value.GetList()) {
      if (!entry.is_dict() || entry.GetDict().size() != 2) return false;
      const auto type = entry.GetDict().FindInt(apps_util::kConditionTypeKey);
      const auto* values = entry.GetDict().FindList(apps_util::kConditionValuesKey);
      if (!type || *type < 0 || *type > 5 || !values) return false;
      for (const auto& condition_value : *values) {
        if (!condition_value.is_dict() || condition_value.GetDict().size() != 2) return false;
        const auto match = condition_value.GetDict().FindInt(apps_util::kMatchTypeKey);
        if (!condition_value.GetDict().FindString(apps_util::kValueKey) ||
            !match || *match < 1 || *match > 7) return false;
      }
    }
  }
  return true;
}

struct Fragment {
  std::string id;
  int order;
  std::string app;
  apps::IntentFilterPtr filter;
  std::vector<std::vector<int>> positions;
};

FileStatus Parse(const base::Value& value, Fragment* out) {
  if (!value.is_dict() || value.GetDict().size() != 5) return FileStatus::kInvalid;
  const auto& dict = value.GetDict();
  const auto* id = dict.FindString("id");
  const auto order = dict.FindInt("order");
  const auto* app = dict.FindString("app_id");
  const auto* filter = dict.FindDict("filter");
  const auto* positions = dict.FindList("positions");
  if (!id || id->empty() || !order || *order < 0 || !app || !filter ||
      !ValidFilter(*filter) || !positions) return FileStatus::kInvalid;
  auto parsed = apps_util::ConvertDictToIntentFilter(filter);
  if (!parsed || positions->size() != parsed->conditions.size()) return FileStatus::kInvalid;
  out->positions.clear();
  for (size_t i = 0; i < positions->size(); ++i) {
    const auto& list = (*positions)[i];
    const auto& condition = *parsed->conditions[i];
    if (!list.is_list() || (Authority(condition) ? list.GetList().size() != condition.condition_values.size()
                                               : !list.GetList().empty())) return FileStatus::kInvalid;
    std::vector<int> indices;
    int last = -1;
    for (const auto& index : list.GetList()) {
      if (!index.is_int() || index.GetInt() <= last) return FileStatus::kInvalid;
      last = index.GetInt();
      indices.push_back(last);
    }
    out->positions.push_back(std::move(indices));
  }
  out->id = *id;
  out->order = *order;
  out->app = *app;
  out->filter = std::move(parsed);
  return FileStatus::kOk;
}

FileStatus Owner(const Fragment& fragment, std::string* out) {
  std::optional<std::string> owner;
  for (const auto& condition : fragment.filter->conditions) {
    if (!Authority(*condition)) continue;
    for (const auto& value : condition->condition_values) {
      const auto current = AuthorityOwner(*value);
      if (owner && *owner != current) return FileStatus::kInvalid;
      owner = current;
    }
  }
  *out = owner.value_or("");
  return FileStatus::kOk;
}
}  // namespace

FileStatus PreferredAppRecordOwner(const base::Value& value, std::string* owner) {
  Fragment fragment;
  const auto status = Parse(value, &fragment);
  return status == FileStatus::kOk ? Owner(fragment, owner) : status;
}

FileStatus EncodePreferredAppRecords(const apps::PreferredApps& entries, base::ListValue* out) {
  if (entries.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return FileStatus::kInvalid;
  base::ListValue fragments;
  for (size_t order = 0; order < entries.size(); ++order) {
    const auto& app = entries[order];
    if (!app || !app->intent_filter) return FileStatus::kInvalid;
    const auto native = apps_util::ConvertIntentFilterToDict(app->intent_filter);
    if (!ValidFilter(native)) return FileStatus::kInvalid;
    std::set<std::string> owners;
    for (const auto& condition : app->intent_filter->conditions) {
      if (!Authority(*condition)) continue;
      if (condition->condition_values.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return FileStatus::kInvalid;
      for (const auto& value : condition->condition_values) owners.insert(AuthorityOwner(*value));
    }
    if (owners.empty()) owners.insert("");
    const auto id = base::UnguessableToken::Create().ToString();
    for (const auto& owner : owners) {
      auto filter = app->intent_filter->Clone();
      base::ListValue positions;
      for (auto& condition : filter->conditions) {
        base::ListValue indices;
        if (Authority(*condition)) {
          apps::ConditionValues selected;
          for (size_t index = 0; index < condition->condition_values.size(); ++index) {
            auto& value = condition->condition_values[index];
            if (AuthorityOwner(*value) != owner) continue;
            indices.Append(static_cast<int>(index));
            selected.push_back(std::move(value));
          }
          condition->condition_values = std::move(selected);
        }
        positions.Append(std::move(indices));
      }
      base::DictValue fragment;
      fragment.Set("id", id);
      fragment.Set("order", static_cast<int>(order));
      fragment.Set("app_id", app->app_id);
      fragment.Set("filter", apps_util::ConvertIntentFilterToDict(filter));
      fragment.Set("positions", std::move(positions));
      fragments.Append(std::move(fragment));
    }
  }
  *out = std::move(fragments);
  return FileStatus::kOk;
}

FileStatus DecodePreferredAppRecords(const base::ListValue& fragments, apps::PreferredApps* out) {
  struct Group {
    int order;
    std::string app;
    apps::IntentFilterPtr common;
    std::vector<std::map<int, apps::ConditionValuePtr>> authorities;
    std::set<std::string> owners;
  };
  std::map<std::string, Group> groups;
  std::map<int, std::string> orders;
  for (const auto& value : fragments) {
    Fragment fragment;
    if (Parse(value, &fragment) != FileStatus::kOk) return FileStatus::kInvalid;
    std::string owner;
    if (Owner(fragment, &owner) != FileStatus::kOk) return FileStatus::kInvalid;
    auto common = fragment.filter->Clone();
    for (auto& condition : common->conditions) if (Authority(*condition)) condition->condition_values.clear();
    auto found = groups.find(fragment.id);
    if (found == groups.end()) {
      if (!orders.emplace(fragment.order, fragment.id).second) return FileStatus::kInvalid;
      Group group;
      group.order = fragment.order;
      group.app = fragment.app;
      group.common = std::move(common);
      group.authorities.resize(fragment.filter->conditions.size());
      found = groups.emplace(fragment.id, std::move(group)).first;
    } else if (found->second.order != fragment.order || found->second.app != fragment.app ||
               *found->second.common != *common) return FileStatus::kInvalid;
    auto& group = found->second;
    if (!group.owners.insert(owner).second) return FileStatus::kInvalid;
    for (size_t i = 0; i < fragment.filter->conditions.size(); ++i) {
      auto& condition = fragment.filter->conditions[i];
      if (!Authority(*condition)) continue;
      for (size_t j = 0; j < condition->condition_values.size(); ++j) {
        if (!group.authorities[i].emplace(fragment.positions[i][j],
            std::move(condition->condition_values[j])).second) return FileStatus::kInvalid;
      }
    }
  }
  apps::PreferredApps entries;
  for (const auto& [order, id] : orders) {
    auto& group = groups.at(id);
    for (size_t i = 0; i < group.common->conditions.size(); ++i) {
      auto& condition = group.common->conditions[i];
      if (!Authority(*condition)) continue;
      for (auto& [index, value] : group.authorities[i]) condition->condition_values.push_back(std::move(value));
    }
    entries.push_back(std::make_unique<apps::PreferredApp>(std::move(group.common), group.app));
  }
  *out = std::move(entries);
  return FileStatus::kOk;
}

}  // namespace alloy::site_records
