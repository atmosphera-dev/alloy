// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <map>
#include <memory>
#include <string>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "chrome/browser/spellchecker/spellcheck_custom_dictionary.h"
#include "components/sync/base/features.h"
#include "components/sync/model/sync_change.h"
#include "components/sync/model/sync_data.h"
#include "components/sync/protocol/entity_specifics.pb.h"
#include "components/sync/test/sync_change_processor_wrapper_for_test.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::site_records {
namespace {

class DictionaryEvents : public SpellcheckCustomDictionary::Observer {
 public:
  void OnCustomDictionaryLoaded() override { ++loads; }
  void OnCustomDictionaryChanged(
      const SpellcheckCustomDictionary::Change&) override {
    ++changes;
  }
  int loads = 0;
  int changes = 0;
};

// Snapshot all native dictionary/backup files, not just the nominal data file.
std::map<base::FilePath, std::string> DictionaryFiles(
    const base::FilePath& root) {
  std::map<base::FilePath, std::string> files;
  base::FileEnumerator enumerator(root, true, base::FileEnumerator::FILES);
  for (auto path = enumerator.Next(); !path.empty(); path = enumerator.Next()) {
    std::string bytes;
    EXPECT_TRUE(base::ReadFileToString(path, &bytes));
    files.emplace(path, std::move(bytes));
  }
  return files;
}

class StatelessSpellingOwnerTest : public testing::TestWithParam<bool> {
 protected:
  content::BrowserTaskEnvironment task_environment_;
};

TEST_P(StatelessSpellingOwnerTest, RefusesLocalAndAccountWordsBeforeAdmission) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatureState(
      syncer::kSpellcheckSeparateLocalAndAccountDictionaries, GetParam());
  base::ScopedTempDir root;
  ASSERT_TRUE(root.CreateUniqueTempDir());

  SpellcheckCustomDictionary parent(root.GetPath());
  parent.Load();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(parent.IsLoaded());
  ASSERT_TRUE(parent.AddWord("parentword"));
  task_environment_.RunUntilIdle();
  const auto before = DictionaryFiles(root.GetPath());
  ASSERT_FALSE(before.empty());

  // Positive control: an ordinary native reader really consumes the seeded
  // file.
  SpellcheckCustomDictionary control(root.GetPath());
  control.Load();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(control.HasWord("parentword"));

  DictionaryEvents events;
  {
    // A real, populated path makes accidental native file loading observable.
    SpellcheckCustomDictionary stateless(
        root.GetPath(), SpellcheckCustomDictionary::Storage::kNone);
    stateless.AddObserver(&events);
    base::test::TestFuture<void> ready;
    stateless.WaitUntilReadyToSync(ready.GetCallback());
    stateless.Load();
    ASSERT_TRUE(ready.Wait());
    ASSERT_TRUE(stateless.IsLoaded());
    EXPECT_EQ(events.loads, 1);
    EXPECT_TRUE(stateless.GetWords().empty());
    EXPECT_FALSE(stateless.HasWord("parentword"));
    EXPECT_FALSE(stateless.AddWord("documentword"));
    EXPECT_FALSE(stateless.RemoveWord("parentword"));
    stateless.Clear();

    sync_pb::EntitySpecifics specifics;
    specifics.mutable_dictionary()->set_word("accountword");
    auto data = syncer::SyncData::CreateLocalData("accountword", "accountword",
                                                  specifics);
    auto processor =
        std::make_unique<syncer::SyncChangeProcessorWrapperForTest>(&parent);
    EXPECT_TRUE(stateless
                    .MergeDataAndStartSyncing(syncer::DICTIONARY, {data},
                                              std::move(processor))
                    .has_value());
    EXPECT_FALSE(stateless.IsSyncing());
    syncer::SyncChangeList changes;
    changes.emplace_back(FROM_HERE, syncer::SyncChange::ACTION_ADD, data);
    EXPECT_TRUE(stateless.ProcessSyncChanges(FROM_HERE, changes).has_value());
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(stateless.GetWords().empty());
    EXPECT_EQ(events.changes, 0);
    EXPECT_EQ(DictionaryFiles(root.GetPath()), before);
    stateless.RemoveObserver(&events);
  }
  task_environment_.RunUntilIdle();
  EXPECT_EQ(DictionaryFiles(root.GetPath()), before);
  EXPECT_TRUE(parent.HasWord("parentword"));
  EXPECT_FALSE(parent.HasWord("accountword"));

  // No backing file is created when no dictionary existed at all, either.
  base::ScopedTempDir empty;
  ASSERT_TRUE(empty.CreateUniqueTempDir());
  {
    SpellcheckCustomDictionary stateless(
        empty.GetPath(), SpellcheckCustomDictionary::Storage::kNone);
    stateless.Load();
    EXPECT_FALSE(stateless.AddWord("freshword"));
  }
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(base::IsDirectoryEmpty(empty.GetPath()));
}

INSTANTIATE_TEST_SUITE_P(LocalAndAccount,
                         StatelessSpellingOwnerTest,
                         testing::Bool());

}  // namespace
}  // namespace alloy::site_records
