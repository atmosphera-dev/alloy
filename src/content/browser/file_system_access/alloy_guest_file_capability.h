// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_CAPABILITY_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_CAPABILITY_H_

#include <optional>
#include <string>
#include <vector>
#include <utility>
#include "base/callback_list.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/memory/weak_ptr.h"
#include "content/common/content_export.h"
#include "content/public/browser/alloy_guest_integration.h"

namespace storage { class FileSystemContext; class FileSystemURL; }
namespace content {
class AlloyGuestFileRegistry;

struct CONTENT_EXPORT AlloyGuestFileMetadata {
  std::string capability, identity, name, mime;
  uint64_t size = 0;
  int64_t modified_ms = 0;
  bool directory = false, readable = false, writable = false;
  std::vector<std::string> ancestors = {};
  static std::optional<AlloyGuestFileMetadata> Parse(const base::DictValue& value);
  base::File::Info FileInfo() const;
};

// Native authority is the selected object plus its original operation lease.
// The synthetic URL is an index into this registry, never an OS-file fallback.
class CONTENT_EXPORT AlloyGuestFileCapability final
    : public base::RefCountedDeleteOnSequence<AlloyGuestFileCapability> {
 public:
  using Result = std::optional<AlloyGuestIntegrationData>;
  using Reply = base::OnceCallback<void(Result)>;
  using Entries = std::vector<scoped_refptr<AlloyGuestFileCapability>>;
  using EntriesReply = base::OnceCallback<void(std::optional<Entries>)>;
  static scoped_refptr<AlloyGuestFileCapability> Create(RenderFrameHost* original,
      AlloyGuestFileMetadata metadata, base::RepeatingCallback<bool()> extra = {});
  static std::optional<Entries> DecodeEntries(RenderFrameHost* original, const Result& result,
      base::RepeatingCallback<bool()> extra = {});
  static void Select(RenderFrameHost* original, base::DictValue details, EntriesReply reply,
      base::RepeatingCallback<bool()> extra = {});
  static scoped_refptr<AlloyGuestFileCapability> FromURL(BrowserContext* context,
      const storage::FileSystemURL& url);
  static std::optional<base::DictValue> Body(const Result& result);
  static bool ValidName(const std::string& name);
  static constexpr uint64_t kChunk = 64 * 1024;

  const AlloyGuestFileMetadata& metadata() const { return metadata_; }
  RenderFrameHost* original() const { return document_.AsRenderFrameHostIfValid(); }
  bool Admitted() const;
  void Retire();
  base::CallbackListSubscription OnRetired(base::OnceClosure callback);
  void Call(std::string operation, base::DictValue details,
      std::vector<std::string> bytes, Reply reply,
      base::RepeatingCallback<bool()> extra = {});
  void Stat(Reply reply, base::RepeatingCallback<bool()> extra = {});
  void Read(uint64_t offset, uint64_t length, int64_t expected_modified_ms,
      Reply reply, base::RepeatingCallback<bool()> extra = {});
  std::optional<Entries> Children(const Result& result);
  bool RegisterURL(BrowserContext* context, storage::FileSystemContext* file_system);
  bool RegisterAlias(BrowserContext* context, const base::FilePath& path);
  void PinSelection();
  void UnpinSelection();
  storage::FileSystemURL IdentityURL(storage::FileSystemContext* context) const;
  std::string UniqueId() const;
  void SetName(std::string name) { metadata_.name = std::move(name); }
  const base::FilePath& virtual_path() const { return virtual_path_; }

 private:
  friend class base::RefCountedDeleteOnSequence<AlloyGuestFileCapability>;
  friend class base::DeleteHelper<AlloyGuestFileCapability>;
  AlloyGuestFileCapability(RenderFrameHost* original, AlloyGuestFileMetadata metadata);
  ~AlloyGuestFileCapability();
  void ReleaseRegistration();
  AlloyGuestFileMetadata metadata_;
  WeakDocumentPtr document_;
  base::WeakPtr<WebContents> contents_;
  base::RepeatingCallback<bool()> admitted_;
  base::RepeatingCallback<bool()> extra_;
  bool live_ = true, released_ = false;
  base::OnceClosureList retired_;
  std::string mount_;
  base::FilePath virtual_path_;
  std::vector<base::FilePath> registered_paths_;
  base::WeakPtr<AlloyGuestFileRegistry> registry_;
  base::WeakPtrFactory<AlloyGuestFileCapability> weak_{this};
};
}  // namespace content
#endif
