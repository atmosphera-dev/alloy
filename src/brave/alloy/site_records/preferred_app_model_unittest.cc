// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "components/services/app_service/public/cpp/preferred_apps_list.h"

#include <map>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "components/services/app_service/public/cpp/intent_filter_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace {
apps::PreferredAppPtr Choice(std::string id, std::string host) {
  return std::make_unique<apps::PreferredApp>(apps_util::MakeIntentFilterForUrlScope(
      GURL("https://" + host + "/")), id);
}
class Observer final : public apps::PreferredAppsListHandle::Observer {
 public:
  explicit Observer(apps::PreferredAppsList* model) : model(model) { model->AddObserver(this); }
  ~Observer() override { if (model) model->RemoveObserver(this); }
  void OnPreferredAppChanged(const std::string& id, bool value) override {
    events.emplace_back(id, value);
    if (changed) changed.Run(id, value);
  }
  void OnPreferredAppsListInitialized() override {
    ++initializations;
    if (initialized) initialized.Run();
  }
  void OnPreferredAppsListWillBeDestroyed(apps::PreferredAppsListHandle* handle) override {
    handle->RemoveObserver(this); model = nullptr;
  }
  apps::PreferredAppsListHandle* model;
  std::vector<std::pair<std::string, bool>> events;
  base::RepeatingCallback<void(std::string, bool)> changed;
  base::RepeatingClosure initialized;
  int initializations = 0;
};
class Delegate final : public apps::PreferredAppsList::Delegate {
 public:
  bool QueryConflict(const std::string&, const apps::IntentFilterPtr&,
                     const std::string&, const apps::IntentFilterPtr&) override {
    return !conflict || conflict.Run();
  }
  bool IsWebAppInExtendedScope(const GURL&, const std::string&) const override {
    return scope && scope.Run();
  }
  base::RepeatingCallback<bool()> conflict;
  base::RepeatingCallback<bool()> scope;
};

TEST(PreferredAppModelTest, ReplacementNotifiesRemovedChangedAndRetainedNativeGrants) {
  apps::PreferredAppsList model(nullptr);
  apps::PreferredApps original;
  original.push_back(Choice("first", "alpha.test"));
  original.push_back(Choice("second", "beta.test"));
  model.Init(std::move(original));
  Observer observer(&model);
  apps::PreferredApps changed;
  changed.push_back(Choice("second", "beta.test"));
  model.Replace(std::move(changed));
  EXPECT_EQ((std::vector<std::pair<std::string, bool>>{{"first", false}}), observer.events);
  EXPECT_FALSE(model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  EXPECT_EQ("second", model.FindPreferredAppForUrl(GURL("https://beta.test/")));
  observer.events.clear();
  changed.push_back(Choice("second", "gamma.test"));
  model.Replace(std::move(changed));
  EXPECT_EQ((std::vector<std::pair<std::string, bool>>{{"second", true}}), observer.events);
  EXPECT_FALSE(model.FindPreferredAppForUrl(GURL("https://beta.test/")));
  EXPECT_EQ("second", model.FindPreferredAppForUrl(GURL("https://gamma.test/")));
}

TEST(PreferredAppModelTest, AdmissionBlocksEveryReadViewWithoutDiscardingRetainedModel) {
  bool admitted = true;
  apps::PreferredAppsList model(nullptr);
  model.SetAvailabilityCallback(base::BindRepeating([](bool* value) { return *value; }, &admitted));
  apps::PreferredApps data;
  data.push_back(Choice("first", "alpha.test"));
  model.Init(std::move(data));
  Observer observer(&model);
  admitted = false;
  EXPECT_FALSE(model.IsInitialized());
  EXPECT_EQ(0u, model.GetEntrySize());
  EXPECT_TRUE(model.GetValue().empty());
  EXPECT_TRUE(model.GetReference().empty());
  EXPECT_FALSE(model.IsPreferredAppForSupportedLinks("first"));
  EXPECT_FALSE(model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  EXPECT_EQ(1u, model.GetPendingValue().size());
  model.SetAvailable(false);
  EXPECT_EQ((std::vector<std::pair<std::string, bool>>{{"first", false}}), observer.events);
  admitted = true;
  EXPECT_FALSE(model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  model.SetAvailable(true);
  EXPECT_EQ("first", model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
}

TEST(PreferredAppModelTest, ReentrantReplacementDrainsCurrentRemovalsAndFreshGrants) {
  apps::PreferredAppsList model(nullptr);
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  data.push_back(Choice("b", "beta.test"));
  model.Init(std::move(data));
  Observer observer(&model);
  observer.changed = base::BindRepeating([](apps::PreferredAppsList* model, std::string id, bool value) {
    if (id == "a" && !value) {
      apps::PreferredApps replacement;
      replacement.push_back(Choice("c", "gamma.test"));
      model->Replace(std::move(replacement));
    }
  }, &model);
  model.Replace({});
  EXPECT_EQ((std::vector<std::pair<std::string, bool>>{{"a", false}, {"b", false}, {"c", true}}), observer.events);
  EXPECT_EQ("c", model.FindPreferredAppForUrl(GURL("https://gamma.test/")));
}

TEST(PreferredAppModelTest, ObserverMayRetireTheModelDuringReplacement) {
  struct Owner {
    apps::PreferredAppsList model{nullptr};
  };
  auto owner = std::make_unique<Owner>();
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  owner->model.Init(std::move(data));
  Observer observer(&owner->model);
  observer.changed = base::BindRepeating([](std::unique_ptr<Owner>* owner,
                                         std::string, bool) { owner->reset(); }, &owner);
  owner->model.Replace({});
  EXPECT_FALSE(owner);
  EXPECT_EQ(nullptr, observer.model);
}

TEST(PreferredAppModelTest, ConflictReentryCannotExposeOrReturnPartialOldMutations) {
  Delegate delegate;
  apps::PreferredAppsList model(&delegate);
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  data.push_back(Choice("b", "alpha.test"));
  model.Init(std::move(data));
  int calls = 0;
  delegate.conflict = base::BindLambdaForTesting([&] {
    // All conflict queries must observe the original complete model. The
    // second query replaces it, retiring the entire interrupted operation.
    EXPECT_EQ(2u, model.GetEntrySize());
    if (++calls == 2) {
      apps::PreferredApps replacement;
      replacement.push_back(Choice("fresh", "beta.test"));
      model.Replace(std::move(replacement));
    }
    return true;
  });
  auto filter = Choice("c", "alpha.test")->intent_filter->Clone();
  EXPECT_TRUE(model.AddPreferredApp("c", filter).empty());
  EXPECT_EQ(2, calls);
  EXPECT_EQ("fresh", model.FindPreferredAppForUrl(GURL("https://beta.test/")));
  EXPECT_FALSE(model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
}

TEST(PreferredAppModelTest, DelegateMayRetireTheOwnerDuringConflictAndMatching) {
  Delegate delegate;
  struct Owner {
    explicit Owner(Delegate* delegate) : model(delegate) {}
    apps::PreferredAppsList model;
  };
  auto owner = std::make_unique<Owner>(&delegate);
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  owner->model.Init(std::move(data));
  delegate.conflict = base::BindLambdaForTesting([&] { owner.reset(); return true; });
  auto filter = Choice("b", "alpha.test")->intent_filter->Clone();
  EXPECT_TRUE(owner->model.AddPreferredApp("b", filter).empty());
  EXPECT_FALSE(owner);
  owner = std::make_unique<Owner>(&delegate);
  data.push_back(Choice("a", "alpha.test"));
  owner->model.Init(std::move(data));
  delegate.scope = base::BindLambdaForTesting([&] { owner.reset(); return false; });
  EXPECT_FALSE(owner->model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  EXPECT_FALSE(owner);
}

TEST(PreferredAppModelTest, ChangedDelegateLookupCannotReturnAnOldGrant) {
  Delegate delegate;
  apps::PreferredAppsList model(&delegate);
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  model.Init(std::move(data));
  delegate.scope = base::BindLambdaForTesting([&] { model.Replace({}); return false; });
  EXPECT_FALSE(model.FindPreferredAppForUrl(GURL("https://alpha.test/")));
  data.push_back(Choice("a", "alpha.test"));
  model.Replace(std::move(data));
  delegate.conflict = base::BindLambdaForTesting([&] { model.Replace({}); return true; });
  apps::IntentFilters filters;
  filters.push_back(Choice("b", "alpha.test")->intent_filter->Clone());
  EXPECT_TRUE(model.FindPreferredAppsForFilters("b", filters).empty());
}

TEST(PreferredAppModelTest, RevokedMutationCannotChangeRetainedDesiredState) {
  apps::PreferredAppsList model(nullptr);
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  model.Init(std::move(data));
  const auto original = model.GetValue();
  model.SetAvailable(false);
  const auto filter = Choice("b", "alpha.test")->intent_filter->Clone();
  EXPECT_TRUE(model.AddPreferredApp("b", filter).empty());
  EXPECT_TRUE(model.DeletePreferredApp("a", filter).empty());
  EXPECT_TRUE(model.DeleteAppId("a").empty());
  EXPECT_TRUE(model.DeleteSupportedLinks("a").empty());
  EXPECT_TRUE(apps::IsEqual(original, model.GetPendingValue()));
  apps::IntentFilters filters;
  filters.push_back(filter->Clone());
  EXPECT_TRUE(model.FindPreferredAppsForFilters(std::nullopt, filters).empty());
}

TEST(PreferredAppModelTest, EveryObserverConvergesAfterAReentrantReplacement) {
  apps::PreferredAppsList model(nullptr);
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  data.push_back(Choice("b", "beta.test"));
  model.Init(std::move(data));
  Observer first(&model), second(&model);
  first.changed = base::BindLambdaForTesting([&](std::string id, bool value) {
    if (id == "a" && !value) {
      apps::PreferredApps fresh;
      fresh.push_back(Choice("c", "gamma.test"));
      model.Replace(std::move(fresh));
    }
  });
  model.Replace({});
  // Both consumers started with a/b enabled. Neither may retain those grants
  // just because the first observer changed the model during their removal.
  for (const auto* observer : {&first, &second}) {
    std::map<std::string, bool> visible{{"a", true}, {"b", true}};
    for (const auto& [id, value] : observer->events) visible[id] = value;
    EXPECT_FALSE(visible["a"]);
    EXPECT_FALSE(visible["b"]);
    EXPECT_TRUE(visible["c"]);
  }
}

TEST(PreferredAppModelTest, NativeLongestPrefixAndExtendedScopeFallbackArePreserved) {
  class ScopeDelegate final : public apps::PreferredAppsList::Delegate {
   public:
    bool QueryConflict(const std::string&, const apps::IntentFilterPtr&,
                       const std::string&, const apps::IntentFilterPtr&) override { return false; }
    bool IsWebAppInExtendedScope(const GURL&, const std::string& id) const override {
      return id == "extended";
    }
  } delegate;
  apps::PreferredAppsList model(&delegate);
  auto scope = [](std::string id, std::string path) {
    return std::make_unique<apps::PreferredApp>(apps_util::MakeIntentFilterForUrlScope(
        GURL("https://alpha.test/" + path)), id);
  };
  apps::PreferredApps data;
  data.push_back(scope("long", "a/b/"));
  data.push_back(scope("short", "a/"));
  model.Init(std::move(data));
  const GURL url("https://alpha.test/a/b/c");
  EXPECT_EQ("short", model.FindPreferredAppForUrl(url));
  model.SetLongestPrefixMatchEnabled(true);
  EXPECT_EQ("long", model.FindPreferredAppForUrl(url));
  data.push_back(scope("extended", "a/b/"));
  data.push_back(scope("normal", "a/"));
  model.Replace(std::move(data));
  EXPECT_EQ("normal", model.FindPreferredAppForUrl(url));
  model.DeleteAppId("normal");
  EXPECT_EQ("extended", model.FindPreferredAppForUrl(url));
}

TEST(PreferredAppModelTest, ObsoletePositiveEventsCannotReachTheNextObserver) {
  apps::PreferredAppsList model(nullptr);
  model.Init();
  Observer first(&model), second(&model);
  first.changed = base::BindLambdaForTesting([&](std::string id, bool value) {
    if (id == "a" && value) model.Replace({});
  });
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  data.push_back(Choice("b", "beta.test"));
  model.Replace(std::move(data));
  ASSERT_FALSE(second.events.empty());
  for (const auto& [id, value] : second.events) EXPECT_FALSE(value) << id;
  EXPECT_EQ(0u, model.GetEntrySize());
}

TEST(PreferredAppModelTest, InitializationReadinessSurvivesAGrantCallbackReplacement) {
  apps::PreferredAppsList model(nullptr);
  Observer first(&model), second(&model);
  first.changed = base::BindLambdaForTesting([&](std::string id, bool value) {
    if (id == "a" && value) {
      apps::PreferredApps fresh;
      fresh.push_back(Choice("b", "beta.test"));
      model.Replace(std::move(fresh));
    }
  });
  apps::PreferredApps data;
  data.push_back(Choice("a", "alpha.test"));
  model.Init(std::move(data));
  EXPECT_TRUE(model.IsInitialized());
  EXPECT_EQ(1, first.initializations);
  EXPECT_EQ(1, second.initializations);
  EXPECT_EQ("b", model.FindPreferredAppForUrl(GURL("https://beta.test/")));
}

TEST(PreferredAppModelTest, InitializationObserverReplacementDoesNotStrandAnotherConsumer) {
  apps::PreferredAppsList model(nullptr);
  Observer first(&model), second(&model);
  first.initialized = base::BindLambdaForTesting([&] {
    apps::PreferredApps fresh;
    fresh.push_back(Choice("b", "beta.test"));
    model.Replace(std::move(fresh));
  });
  model.Init();
  EXPECT_EQ(1, first.initializations);
  EXPECT_EQ(1, second.initializations);
  EXPECT_EQ("b", model.FindPreferredAppForUrl(GURL("https://beta.test/")));
}
}  // namespace
