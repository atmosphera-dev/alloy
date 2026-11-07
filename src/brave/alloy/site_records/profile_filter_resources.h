// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_FILTER_RESOURCES_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_FILTER_RESOURCES_H_
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "brave/alloy/site_records/profile_filter_storage.h"
namespace alloy::site_records {
class ProfileFilterResources final : public base::RefCountedDeleteOnSequence<ProfileFilterResources> {
 public:
  ProfileFilterResources(std::shared_ptr<ProfileFilterStorage> storage,
      scoped_refptr<base::SequencedTaskRunner> runner);
  struct Result {
    Result() = default;
    Result(Result&&) = default;
    ~Result();
    FileStatus status = FileStatus::kOk;
    base::Value values{base::Value::Type::LIST};
  };
  Result Read();
  FileStatus Write(base::Value values);
  void Close();
 private:
  friend class base::RefCountedDeleteOnSequence<ProfileFilterResources>;
  friend class base::DeleteHelper<ProfileFilterResources>;
  ~ProfileFilterResources();
  FileStatus Open();
  const std::shared_ptr<ProfileFilterStorage> storage_;
  std::unique_ptr<value_store::ValueStore> values_;
  bool closed_ = false;
};
}  // namespace alloy::site_records
#endif
