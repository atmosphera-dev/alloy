// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
#include "content/browser/file_system_access/alloy_guest_uploads.h"
#include "content/browser/file_system_access/alloy_guest_files.h"
#include "content/browser/file_system_access/alloy_guest_file_binding.h"
#include "content/browser/file_system_access/fixed_file_system_access_permission_grant.h"
#include "cef/libcef/browser/osr/alloy_guest_drag_sequence.h"
#include <fcntl.h>
#include <atomic>
#include <linux/magic.h>
#include <sys/vfs.h>
#include <unistd.h>
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/task/thread_pool.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/test_future.h"
#include "base/unguessable_token.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/file_access/scoped_file_access.h"
#include "components/file_access/scoped_file_access_delegate.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/render_process_host.h"
#include "content/browser/blob_storage/file_backed_blob_factory_frame_impl.h"
#include "content/browser/security/cpsp/child_process_security_policy_impl.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/web_contents/file_chooser_impl.h"
#include "content/public/test/test_utils.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "third_party/blink/public/mojom/blob/blob.mojom.h"
#include "third_party/blink/public/mojom/blob/data_element.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_data_transfer_token.mojom.h"
#include "content/public/test/web_contents_tester.h"
#include "storage/browser/file_system/file_system_context.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::guest::integration {
namespace {
using Cap = content::AlloyGuestFileCapability;
using Metadata = content::AlloyGuestFileMetadata;
Metadata Entry() {
  return {base::UnguessableToken::Create().ToString(), base::UnguessableToken::Create().ToString(),
          "selected.txt", "text/plain", 6, 1234, false, true, true};
}
class FileProvider final : public content::AlloyGuestIntegration {
 public:
  std::vector<Metadata> selected;
  int reads = 0;
  bool live = true;
  std::vector<base::OnceClosure> retired;
  std::vector<std::string> releases;
  void Request(content::RenderFrameHost*, Operation operation,
    content::AlloyGuestIntegrationData data, Reply reply) override {
    if (operation == Operation::kSelectFiles) {
      base::ListValue entries;
      for (const auto& meta : selected) {
        base::DictValue entry;
        entry.Set("capability", meta.capability); entry.Set("identity", meta.identity);
        entry.Set("name", meta.name); entry.Set("mime", meta.mime);
        entry.Set("size", std::to_string(meta.size)); entry.Set("modified-ms", std::to_string(meta.modified_ms));
        entry.Set("directory", false); entry.Set("readable", true); entry.Set("writable", false);
        entry.Set("ancestors", base::ListValue()); entries.Append(std::move(entry));
      }
      base::DictValue body; body.Set("entries", std::move(entries));
      content::AlloyGuestIntegrationData result; result.details.Set("status", "ok");
      std::string bytes; EXPECT_TRUE(base::JSONWriter::Write(body, &bytes)); result.bytes.push_back(bytes);
      std::move(reply).Run(std::move(result)); return;
    }
    EXPECT_EQ(operation, Operation::kFileRead);
    EXPECT_TRUE(data.admitted && data.admitted.Run());
    content::AlloyGuestIntegrationData result;
    result.details.Set("status", "ok");
    if (*data.details.FindString("operation") == "read") {
      ++reads;
      uint64_t offset = 0, length = 0;
      EXPECT_TRUE(base::StringToUint64(*data.details.FindString("offset"), &offset));
      EXPECT_TRUE(base::StringToUint64(*data.details.FindString("length"), &length));
      result.bytes.push_back(std::string("abcdef").substr(offset, length));
    } else {
      base::DictValue metadata; metadata.Set("size", "6"); metadata.Set("modified-ms", "1234");
      std::string body; EXPECT_TRUE(base::JSONWriter::Write(metadata, &body));
      result.bytes.push_back(body);
    }
    std::move(reply).Run(std::move(result));
  }
  base::RepeatingCallback<bool()> CaptureAdmission(content::RenderFrameHost*, bool) override {
    return base::BindRepeating([](bool* live) { return *live; }, &live);
  }
  base::RepeatingCallback<bool()> CaptureLifetime(content::RenderFrameHost* original,
      base::OnceClosure callback) override {
    retired.push_back(std::move(callback)); return CaptureAdmission(original, true);
  }
  void RetireFile(const std::string& capability) override { releases.push_back(capability); }
  void Revoke() {
    live = false; auto callbacks = std::move(retired);
    for (auto& callback : callbacks) std::move(callback).Run();
  }
};
class GuestNativeFilesTest : public ChromeRenderViewHostTestHarness {};

struct SnapshotAccessState {
  std::atomic<bool> allowed{false};
  std::atomic<int> reads{0};
  std::vector<GURL> destinations;
};
class SnapshotAccess final : public file_access::ScopedFileAccessDelegate {
 public:
  explicit SnapshotAccess(std::shared_ptr<SnapshotAccessState> state) : state_(std::move(state)) {}
  ~SnapshotAccess() override = default;
  void RequestFilesAccess(const std::vector<base::FilePath>&, const GURL&,
      base::OnceCallback<void(file_access::ScopedFileAccess)> callback) override {
    std::move(callback).Run(file_access::ScopedFileAccess::Denied());
  }
  void RequestFilesAccessForSystem(const std::vector<base::FilePath>&,
      base::OnceCallback<void(file_access::ScopedFileAccess)> callback) override {
    std::move(callback).Run(file_access::ScopedFileAccess::Denied());
  }
  void RequestDefaultFilesAccess(const std::vector<base::FilePath>&,
      base::OnceCallback<void(file_access::ScopedFileAccess)> callback) override {
    std::move(callback).Run(file_access::ScopedFileAccess::Denied());
  }
  RequestFilesAccessIOCallback CreateFileAccessCallback(const GURL& destination) const override {
    state_->destinations.push_back(destination);
    return base::BindRepeating([](std::shared_ptr<SnapshotAccessState> state,
        const std::vector<base::FilePath>&, base::OnceCallback<void(file_access::ScopedFileAccess)> callback) {
      ++state->reads;
      // Match the real DlpScopedFileAccessDelegate's queued IO reply. Inline
      // denial reenters the pinned BlobReader before Read installs its callback.
      content::GetIOThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce(std::move(callback),
          state->allowed ? file_access::ScopedFileAccess::Allowed() : file_access::ScopedFileAccess::Denied()));
    }, state_);
  }
 private:
  std::shared_ptr<SnapshotAccessState> state_;
};
class SnapshotBlobRead final : public mojo::DataPipeDrainer::Client {
 public:
  std::string Read(blink::mojom::Blob& blob) {
    mojo::ScopedDataPipeProducerHandle producer; mojo::ScopedDataPipeConsumerHandle consumer;
    CHECK_EQ(mojo::CreateDataPipe(nullptr, producer, consumer), MOJO_RESULT_OK);
    blob.ReadAll(std::move(producer), mojo::NullRemote());
    mojo::DataPipeDrainer drain(this, std::move(consumer));
    EXPECT_TRUE(complete_.Wait()); return bytes_;
  }
  void OnDataAvailable(base::span<const uint8_t> bytes) override { bytes_.append(bytes.begin(), bytes.end()); }
  void OnDataComplete() override { complete_.SetValue(); }
 private:
  std::string bytes_;
  base::test::TestFuture<void> complete_;
};

TEST_F(GuestNativeFilesTest, NativeSnapshotBlobPreservesDestinationFileAccessChecks) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get();
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame(); auto cap = Cap::Create(frame, Entry());
  base::test::TestFuture<std::optional<content::AlloyGuestUploadFiles>> result;
  content::SnapshotAlloyGuestFiles(frame, {cap}, false, result.GetCallback());
  auto selected = result.Take(); ASSERT_TRUE(selected); const auto path = selected->files[0].path;
  ASSERT_FALSE(file_access::ScopedFileAccessDelegate::HasInstance());
  auto state = std::make_shared<SnapshotAccessState>(); SnapshotAccess access(state);
  auto snapshot = content::CreateAlloyGuestUploadBlob(frame, path); ASSERT_TRUE(snapshot);
  EXPECT_EQ(state->destinations, std::vector<GURL>{frame->GetLastCommittedURL()});
  mojo::Remote<blink::mojom::Blob> blob(std::move(snapshot->blob));
  EXPECT_TRUE(SnapshotBlobRead().Read(*blob).empty());
  EXPECT_GE(state->reads.load(), 1);
  state->allowed = true;
  EXPECT_EQ(SnapshotBlobRead().Read(*blob), "abcdef");
  EXPECT_GE(state->reads.load(), 2);
  selected.reset(); content::RunAllTasksUntilIdle(); EXPECT_TRUE(base::PathExists(path));
  blob.reset(); content::RunAllTasksUntilIdle(); EXPECT_FALSE(base::PathExists(path));
  app->Revoke();
}

class SelectedFiles final : public content::FileSelectListener {
 public:
  std::vector<blink::mojom::FileChooserFileInfoPtr> files;
  base::test::TestFuture<void> ready;
  void FileSelected(std::vector<blink::mojom::FileChooserFileInfoPtr> result,
      const base::FilePath&, blink::mojom::FileChooserParams::Mode) override {
    files = std::move(result); ready.SetValue();
  }
  void FileSelectionCanceled() override { ready.SetValue(); }
 private:
  ~SelectedFiles() override = default;
};

TEST_F(GuestNativeFilesTest, RoundTwoDiscardedNativeChooserDeliveryReleasesBacking) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get(); app->selected = {Entry()};
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto listener = base::MakeRefCounted<SelectedFiles>();
  blink::mojom::FileChooserParams params;
  content::SelectAlloyGuestUploadFiles(frame, params, listener);
  ASSERT_TRUE(listener->ready.Wait()); ASSERT_EQ(listener->files.size(), 1u);
  const auto path = listener->files[0]->get_native_file()->file_path;
  ASSERT_TRUE(base::PathExists(path));
  listener->files.clear(); content::RunAllTasksUntilIdle();
  EXPECT_FALSE(base::PathExists(path));
  EXPECT_FALSE(content::GetAlloyGuestUploadProof(frame, path));
  EXPECT_TRUE(app->live); app->Revoke();
}

TEST_F(GuestNativeFilesTest, RoundTwoPartiallyFilteredSelectionDoesNotPinTheLastBlob) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get();
  auto other = Entry(); other.name = "other.txt"; app->selected = {Entry(), other};
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto listener = base::MakeRefCounted<SelectedFiles>();
  blink::mojom::FileChooserParams params; params.mode = blink::mojom::FileChooserParams::Mode::kOpenMultiple;
  content::SelectAlloyGuestUploadFiles(frame, params, listener);
  ASSERT_TRUE(listener->ready.Wait()); ASSERT_EQ(listener->files.size(), 2u);
  const auto accepted = listener->files[0]->get_native_file()->file_path;
  const auto discarded = listener->files[1]->get_native_file()->file_path;
  listener->files.erase(listener->files.begin() + 1);
  content::ChildProcessSecurityPolicyImpl::GetInstance()->GrantReadFile(frame->GetProcess()->GetID(), accepted);
  mojo::AssociatedRemote<blink::mojom::FileBackedBlobFactory> factory;
  content::FileBackedBlobFactoryFrameImpl::CreateForCurrentDocument(frame, factory.BindNewEndpointAndPassDedicatedReceiver());
  mojo::Remote<blink::mojom::Blob> blob; base::test::TestFuture<void> registered;
  factory->RegisterBlobSync(blob.BindNewPipeAndPassReceiver(), base::UnguessableToken::Create().ToString(),
      "text/plain", blink::mojom::DataElementFile::New(accepted, 0, 6, std::nullopt), registered.GetCallback());
  ASSERT_TRUE(registered.Wait()); listener->files.clear(); content::RunAllTasksUntilIdle();
  std::string bytes; EXPECT_TRUE(base::ReadFileToString(accepted, &bytes)); EXPECT_EQ(bytes, "abcdef");
  blob.reset(); content::RunAllTasksUntilIdle();
  EXPECT_FALSE(base::PathExists(accepted)); EXPECT_FALSE(base::PathExists(discarded));
  EXPECT_TRUE(app->live); app->Revoke();
}

TEST_F(GuestNativeFilesTest, RoundTwoNativeFileCopyOutlivesSourceHandoffUntilConsumption) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get();
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame(); auto cap = Cap::Create(frame, Entry());
  base::test::TestFuture<std::optional<content::AlloyGuestUploadFiles>> result;
  content::SnapshotAlloyGuestFiles(frame, {cap}, false, result.GetCallback());
  auto source = result.Take(); ASSERT_TRUE(source); ASSERT_EQ(source->files.size(), 1u);
  auto delivered = source->files; const auto path = delivered[0].path;
  source.reset(); content::RunAllTasksUntilIdle();
  ASSERT_TRUE(content::GetAlloyGuestUploadProof(frame, path));
  auto consumer = content::AcquireAlloyGuestUploadConsumer(frame, path); ASSERT_TRUE(consumer);
  delivered.clear(); content::RunAllTasksUntilIdle();
  std::string bytes; EXPECT_TRUE(base::ReadFileToString(path, &bytes)); EXPECT_EQ(bytes, "abcdef");
  std::move(consumer).Run(); content::RunAllTasksUntilIdle();
  EXPECT_FALSE(base::PathExists(path)); EXPECT_TRUE(app->live); app->Revoke();
}

TEST_F(GuestNativeFilesTest, RoundTwoDisconnectedNativeChooserDropsDeliveredBacking) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get(); app->selected = {Entry()};
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto [chooser, remote] = content::FileChooserImpl::CreateForTesting(content::RenderFrameHostImpl::From(frame));
  auto native_listener = base::MakeRefCounted<content::FileChooserImpl::FileSelectListenerImpl>(chooser);
  native_listener->SetFullscreenBlock(base::ScopedClosureRunner(base::DoNothing()));
  auto selected = base::MakeRefCounted<SelectedFiles>(); blink::mojom::FileChooserParams params;
  content::SelectAlloyGuestUploadFiles(frame, params, selected);
  ASSERT_TRUE(selected->ready.Wait()); ASSERT_EQ(selected->files.size(), 1u);
  const auto path = selected->files[0]->get_native_file()->file_path;
  remote.reset(); content::RunAllTasksUntilIdle();
  native_listener->FileSelected(std::move(selected->files), {}, params.mode);
  content::RunAllTasksUntilIdle();
  EXPECT_FALSE(base::PathExists(path)); EXPECT_FALSE(content::GetAlloyGuestUploadProof(frame, path));
  EXPECT_TRUE(app->live); app->Revoke();
}

TEST_F(GuestNativeFilesTest, ScopedLookupAndReadsRetireWithoutFilesystemFallback) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>();
  auto* app = provider.get();
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto entry = Entry(); const auto token = entry.capability;
  auto cap = Cap::Create(web_contents()->GetPrimaryMainFrame(), std::move(entry));
  ASSERT_TRUE(cap); ASSERT_TRUE(cap->Admitted());
  auto* context = web_contents()->GetPrimaryMainFrame()->GetStoragePartition()->GetFileSystemContext();
  ASSERT_TRUE(cap->RegisterURL(profile(), context));
  auto url = context->CreateCrackedFileSystemURL(blink::StorageKey(),
      storage::kFileSystemTypeExternal, cap->virtual_path());
  ASSERT_TRUE(url.is_valid());
  EXPECT_EQ(Cap::FromURL(profile(), url).get(), cap.get());
  base::test::TestFuture<Cap::Result> result;
  cap->Read(1, 3, 1234, result.GetCallback());
  ASSERT_TRUE(result.Get()); ASSERT_EQ(result.Get()->bytes.size(), 1u);
  EXPECT_EQ(result.Get()->bytes[0], "bcd"); EXPECT_EQ(app->reads, 1);
  app->Revoke();
  EXPECT_FALSE(cap->Admitted()); EXPECT_FALSE(Cap::FromURL(profile(), url));
  base::test::TestFuture<Cap::Result> late;
  cap->Read(0, 1, 1234, late.GetCallback());
  EXPECT_FALSE(late.Get()); EXPECT_EQ(app->reads, 1);
  ASSERT_EQ(app->releases.size(), 1u); EXPECT_EQ(app->releases[0], token);
  cap->Retire(); EXPECT_EQ(app->releases.size(), 1u);
}

TEST_F(GuestNativeFilesTest, PathOnlyUploadsAreSealedRamAndOriginalDocumentOwned) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get();
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto cap = Cap::Create(frame, Entry()); ASSERT_TRUE(cap);
  base::test::TestFuture<std::optional<content::AlloyGuestUploadFiles>> result;
  content::SnapshotAlloyGuestFiles(frame, {cap}, false, result.GetCallback());
  ASSERT_TRUE(result.Get()); ASSERT_EQ(result.Get()->files.size(), 1u);
  const auto path = result.Get()->files[0].path;
  auto proof = content::GetAlloyGuestUploadProof(frame, path); ASSERT_TRUE(proof);
  ASSERT_TRUE(proof->admitted.Run());
  EXPECT_FALSE(content::GetAlloyGuestUploadProof(frame, base::FilePath("/unselected")));
  auto verify = [&proof] {
    base::test::TestFuture<bool> checked;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](base::RepeatingCallback<bool()> verify) { return verify.Run(); }, proof->verify), checked.GetCallback());
    return checked.Get();
  };
  EXPECT_TRUE(verify());
  std::string bytes; ASSERT_TRUE(base::ReadFileToString(path, &bytes)); EXPECT_EQ(bytes, "abcdef");
  {
    base::ScopedFD fd(open(path.value().c_str(), O_RDWR | O_CLOEXEC)); ASSERT_TRUE(fd.is_valid());
    struct statfs fs; ASSERT_EQ(fstatfs(fd.get(), &fs), 0); EXPECT_EQ(fs.f_type, TMPFS_MAGIC);
    EXPECT_EQ(fcntl(fd.get(), F_GET_SEALS), F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
    EXPECT_EQ(pwrite(fd.get(), "X", 1, 0), -1); EXPECT_EQ(errno, EPERM);
    EXPECT_EQ(ftruncate(fd.get(), 0), -1); EXPECT_EQ(errno, EPERM);
  }
  base::FilePath target; ASSERT_TRUE(base::ReadSymbolicLink(path, &target));
  base::ScopedTempDir other; ASSERT_TRUE(other.CreateUniqueTempDir());
  const auto unrelated = other.GetPath().AppendASCII("unselected.txt");
  ASSERT_TRUE(base::WriteFile(unrelated, "unselected"));
  ASSERT_TRUE(base::DeleteFile(path)); ASSERT_TRUE(base::CreateSymbolicLink(unrelated, path));
  EXPECT_FALSE(verify());  // A known alias string never grants a different inode.
  ASSERT_TRUE(base::DeleteFile(path)); ASSERT_TRUE(base::CreateSymbolicLink(target, path));
  EXPECT_TRUE(verify());
  app->Revoke(); task_environment()->RunUntilIdle();
  EXPECT_FALSE(proof->admitted.Run()); EXPECT_FALSE(verify());
  EXPECT_FALSE(base::PathExists(path));
}

TEST_F(GuestNativeFilesTest, ReviewUploadBackingEndsAtLastNativeBlobConsumerNotDocumentEnd) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get();
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto cap = Cap::Create(frame, Entry()); ASSERT_TRUE(cap);
  base::test::TestFuture<std::optional<content::AlloyGuestUploadFiles>> result;
  content::SnapshotAlloyGuestFiles(frame, {cap}, false, result.GetCallback());
  ASSERT_TRUE(result.Get()); const auto path = result.Get()->files[0].path;
  auto selection = result.Take();
  auto* manager = static_cast<content::FileSystemAccessManagerImpl*>(
      frame->GetStoragePartition()->GetFileSystemAccessEntryFactory());
  const auto url = manager->context()->CreateCrackedFileSystemURL(
      blink::StorageKey(), storage::kFileSystemTypeLocal, path);
  mojo::Remote<blink::mojom::FileSystemAccessDataTransferToken> token, token_clone;
  manager->CreateFileSystemAccessDataTransferToken(content::PathInfo(path),
      frame->GetProcess()->GetID().GetUnsafeValue(), token.BindNewPipeAndPassReceiver());
  token->Clone(token_clone.BindNewPipeAndPassReceiver()); token.FlushForTesting();
  token.reset(); cap.reset();
  content::ChildProcessSecurityPolicyImpl::GetInstance()->GrantReadFile(frame->GetProcess()->GetID(), path);
  mojo::AssociatedRemote<blink::mojom::FileBackedBlobFactory> factory;
  content::FileBackedBlobFactoryFrameImpl::CreateForCurrentDocument(frame, factory.BindNewEndpointAndPassDedicatedReceiver());
  mojo::Remote<blink::mojom::Blob> blob, clone;
  base::test::TestFuture<void> registered;
  factory->RegisterBlobSync(blob.BindNewPipeAndPassReceiver(), base::UnguessableToken::Create().ToString(),
      "text/plain", blink::mojom::DataElementFile::New(path, 0, 6, std::nullopt), registered.GetCallback());
  ASSERT_TRUE(registered.Wait());
  // Release the source handoff only after the real native Blob acquired it.
  selection.reset();
  blob->Clone(clone.BindNewPipeAndPassReceiver()); blob.FlushForTesting();
  blob.reset(); content::RunAllTasksUntilIdle();
  EXPECT_TRUE(base::PathExists(path)); EXPECT_TRUE(app->live);
  clone.reset(); content::RunAllTasksUntilIdle(); task_environment()->RunUntilIdle();
  EXPECT_FALSE(base::PathExists(path));
  EXPECT_FALSE(content::GetAlloyGuestUploadProof(frame, path));
  {
    auto retained = Cap::FromURL(profile(), url);
    EXPECT_TRUE(retained);
    if (retained) EXPECT_TRUE(retained->Admitted());
  }
  token_clone.reset(); content::RunAllTasksUntilIdle();
  EXPECT_FALSE(Cap::FromURL(profile(), url));
  EXPECT_TRUE(app->live);
  app->Revoke(); task_environment()->RunUntilIdle();
}

TEST_F(GuestNativeFilesTest, ReviewSequenceOwnedFactoriesAdoptTheirInitialReference) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  auto provider = std::make_unique<FileProvider>(); auto* app = provider.get();
  content::AlloyGuestIntegration::Install(web_contents(), std::move(provider));
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto cap = Cap::Create(frame, Entry()); ASSERT_TRUE(cap);
  EXPECT_TRUE(cap->HasOneRef());
  auto binding = content::AlloyGuestFileBinding::Create(frame, cap); ASSERT_TRUE(binding);
  EXPECT_TRUE(binding->HasOneRef());
  EXPECT_FALSE(cap->HasOneRef());
  binding.reset(); content::RunAllTasksUntilIdle();
  EXPECT_TRUE(cap->HasOneRef());
  app->Revoke();
}

TEST(GuestFileMetadata, RejectsPathAuthorityAndNativeExtentOverflow) {
  base::DictValue value;
  const auto entry = Entry();
  value.Set("capability", entry.capability); value.Set("identity", entry.identity);
  value.Set("name", entry.name); value.Set("mime", entry.mime);
  value.Set("ancestors", base::ListValue());
  value.Set("size", "6"); value.Set("modified-ms", "1234");
  value.Set("directory", false); value.Set("readable", true); value.Set("writable", true);
  EXPECT_TRUE(Metadata::Parse(value));
  value.Set("name", "../outside"); EXPECT_FALSE(Metadata::Parse(value));
  value.Set("name", entry.name); value.Set("size", "18446744073709551615");
  EXPECT_FALSE(Metadata::Parse(value));
  value.Set("size", "6"); value.Set("capability", "/host/path");
  EXPECT_FALSE(Metadata::Parse(value));
}

TEST_F(GuestNativeFilesTest, OpaqueLineageKeepsRepeatedSelectionsAndParentLocksConsistent) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  content::AlloyGuestIntegration::Install(web_contents(), std::make_unique<FileProvider>());
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto parent = Entry(); parent.directory = true; parent.size = 0; parent.name = "folder";
  auto child = Entry(); child.ancestors = {parent.identity};
  auto cap = Cap::Create(frame, child); ASSERT_TRUE(cap);
  auto root = Cap::Create(frame, parent); ASSERT_TRUE(root);
  auto* context = frame->GetStoragePartition()->GetFileSystemContext();
  EXPECT_TRUE(root->IdentityURL(context).path().IsParent(cap->IdentityURL(context).path()));
  child.capability = base::UnguessableToken::Create().ToString();
  auto repeated = Cap::Create(frame, child); ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->IdentityURL(context), cap->IdentityURL(context));
  EXPECT_EQ(repeated->UniqueId(), cap->UniqueId());
  ASSERT_EQ(cap->UniqueId().size(), 36u); EXPECT_EQ(cap->UniqueId()[14], '4');
  child.ancestors.clear();
  EXPECT_FALSE(Cap::Create(frame, child));
  parent.directory = false;
  EXPECT_FALSE(Cap::Create(frame, parent));
}

TEST_F(GuestNativeFilesTest, RemintingCannotTurnAnEndedIssuingBindingIntoANewGrant) {
  content::WebContentsTester::For(web_contents())->NavigateAndCommit(GURL("https://files.test/"));
  content::AlloyGuestIntegration::Install(web_contents(), std::make_unique<FileProvider>());
  auto* frame = web_contents()->GetPrimaryMainFrame();
  auto cap = Cap::Create(frame, Entry()); ASSERT_TRUE(cap);
  auto* context = frame->GetStoragePartition()->GetFileSystemContext();
  ASSERT_TRUE(cap->RegisterURL(profile(), context));
  const auto url = context->CreateCrackedFileSystemURL(frame->GetStorageKey(),
      storage::kFileSystemTypeExternal, cap->virtual_path());
  auto issuer = content::AlloyGuestFileBinding::Create(frame, cap); ASSERT_TRUE(issuer);
  issuer->Retire(); ASSERT_TRUE(cap->Admitted()); ASSERT_FALSE(issuer->Admitted());
  auto grant = base::MakeRefCounted<content::FixedFileSystemAccessPermissionGrant>(
      blink::mojom::PermissionStatus::GRANTED, content::PathInfo(url.path()));
  using Manager = content::FileSystemAccessManagerImpl;
  Manager::SharedHandleState state(grant, grant); state.guest_file = cap; state.guest_binding = issuer;
  auto* manager = static_cast<Manager*>(frame->GetStoragePartition()->GetFileSystemAccessEntryFactory());
  Manager::BindingContext binding(frame->GetStorageKey(), frame->GetLastCommittedURL(), frame->GetGlobalId());
  auto handle = content::CreateAlloyGuestFileHandle(manager, binding, url, cap->metadata().name, state);
  base::test::TestFuture<blink::mojom::PermissionStatus> permission;
  handle->GetPermissionStatus(blink::mojom::FileSystemAccessPermissionMode::kRead, permission.GetCallback());
  EXPECT_EQ(permission.Get(), blink::mojom::PermissionStatus::DENIED);
}
TEST(GuestDragSequence, DropWaitsForTheLatestNativeReplyEvenWhenRepliesReorder) {
  AlloyGuestDragSequence sequence;
  auto enter = sequence.Reply(); auto over = sequence.Reply();
  bool dropped = false;
  EXPECT_TRUE(sequence.DeferDropIfPending(base::BindOnce([](bool* flag) { *flag = true; }, &dropped)));
  std::move(over).Run(true); EXPECT_FALSE(dropped);
  std::move(enter).Run(false); EXPECT_TRUE(dropped);
  ASSERT_TRUE(sequence.Handling()); EXPECT_TRUE(*sequence.Handling());
}

TEST(GuestDragSequence, RetirementCancelsDropAndOldRepliesCannotReviveIt) {
  AlloyGuestDragSequence sequence;
  auto old = sequence.Reply(); bool old_drop = false, new_drop = false;
  EXPECT_TRUE(sequence.DeferDropIfPending(base::BindOnce([](bool* flag) { *flag = true; }, &old_drop)));
  sequence.Reset();
  auto fresh = sequence.Reply();
  EXPECT_TRUE(sequence.DeferDropIfPending(base::BindOnce([](bool* flag) { *flag = true; }, &new_drop)));
  std::move(old).Run(true); EXPECT_FALSE(old_drop); EXPECT_FALSE(new_drop);
  std::move(fresh).Run(false); EXPECT_FALSE(old_drop); EXPECT_TRUE(new_drop);
  ASSERT_TRUE(sequence.Handling()); EXPECT_FALSE(*sequence.Handling());
}

TEST(GuestDragSequence, LostRendererReplyCancelsRatherThanInventingADropDecision) {
  AlloyGuestDragSequence sequence;
  auto reply = sequence.Reply(); bool dropped = false;
  EXPECT_TRUE(sequence.DeferDropIfPending(base::BindOnce([](bool* flag) { *flag = true; }, &dropped)));
  reply.Reset(); EXPECT_FALSE(dropped); EXPECT_FALSE(sequence.Handling());
}

TEST(GuestDragSequence, AlreadyAcknowledgedTargetNeedsNoDeferral) {
  AlloyGuestDragSequence sequence;
  auto reply = sequence.Reply(); std::move(reply).Run(true);
  EXPECT_FALSE(sequence.DeferDropIfPending(base::DoNothing()));
  ASSERT_TRUE(sequence.Handling()); EXPECT_TRUE(*sequence.Handling());
}
}  // namespace
}  // namespace alloy::guest::integration
