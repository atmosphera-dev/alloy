// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/values.h"
#include "brave/alloy/site_records/storage_mode.h"
#include "chrome/browser/extensions/chrome_extension_system.h"
#include "chrome/browser/extensions/chrome_extension_system_factory.h"
#include "chrome/browser/file_system_access/chrome_file_system_access_permission_context.h"
#include "chrome/browser/file_system_access/file_system_access_permission_context_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/browser/file_system_access/file_system_access_manager_impl.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/file_system_access_permission_context.h"
#include "content/public/browser/file_system_access_permission_grant.h"
#include "content/public/test/web_contents_tester.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "storage/browser/file_system/file_system_context.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom.h"
#include "third_party/blink/public/mojom/frame/user_activation_update_types.mojom.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

std::optional<std::string> ReadNativeRequest(int fd) {
  const auto deadline = base::TimeTicks::Now() + base::Seconds(15);
  std::string line;
  while (line.size() < 4096) {
    const auto remaining = deadline - base::TimeTicks::Now();
    if (remaining <= base::TimeDelta()) return std::nullopt;
    pollfd event{fd, POLLIN, 0};
    int ready = poll(&event, 1, static_cast<int>(remaining.InMilliseconds()));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) return std::nullopt;
    char byte;
    const auto size = read(fd, &byte, 1);
    if (size < 0 && errno == EINTR) continue;
    if (size != 1) return std::nullopt;
    if (byte == '\n') return line;
    line.push_back(byte);
  }
  return std::nullopt;
}

bool SendNativeReply(int fd, const base::DictValue& reply) {
  auto json = base::WriteJson(reply);
  if (!json) return false;
  json->push_back('\n');
  size_t offset = 0;
  while (offset < json->size()) {
    const auto size = write(fd, json->data() + offset, json->size() - offset);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) return false;
    offset += static_cast<size_t>(size);
  }
  return true;
}

class FixtureGestureTest : public ChromeRenderViewHostTestHarness {};

TEST_F(FixtureGestureTest, NativeAcknowledgementOrdersGestureBeforePicker) {
  const auto tools = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      "alloy-probe-tools-dir");
  ASSERT_FALSE(tools.empty());
  auto* child = TestingProfile::Builder().BuildOffTheRecord(
      profile(), Profile::OTRProfileID::CreateUniqueForAlloyStateless(
                     NetworkState::kKeep));
  extensions::ChromeExtensionSystemFactory::GetInstance()->SetTestingFactoryAndUse(
      child, base::BindRepeating([](content::BrowserContext* context)
                                    -> std::unique_ptr<KeyedService> {
        return std::make_unique<extensions::ChromeExtensionSystem>(
            Profile::FromBrowserContext(context));
      }));
  auto contents = content::WebContentsTester::CreateTestWebContents(child, nullptr);
  const GURL url("https://fixture-gesture.test/page");
  content::WebContentsTester::For(contents.get())->NavigateAndCommit(url);
  auto old_document = contents->GetPrimaryMainFrame()->GetWeakDocumentPtr();
  content::WebContentsTester::For(contents.get())->NavigateAndCommit(url);
  EXPECT_EQ(old_document.AsRenderFrameHostIfValid(), nullptr);
  auto* frame = static_cast<content::RenderFrameHostImpl*>(contents->GetPrimaryMainFrame());
  frame->UpdateUserActivationState(blink::mojom::UserActivationUpdateType::kClearActivation,
                                  blink::mojom::UserActivationNotificationType::kNone);
  ASSERT_FALSE(frame->HasTransientUserActivation());

  auto* partition = static_cast<content::StoragePartitionImpl*>(frame->GetStoragePartition());
  auto* permission_root = FileSystemAccessPermissionContextFactory::GetForProfile(child);
  ASSERT_NE(permission_root, nullptr);
  // TestingProfile inherits BrowserContext's null permission-context getter.
  // Compose the real manager as production StoragePartitionImpl does, using
  // the actual native Chrome context instead of that test-profile null owner.
  auto manager = base::MakeRefCounted<content::FileSystemAccessManagerImpl>(
      base::WrapRefCounted(partition->GetFileSystemContext()),
      base::WrapRefCounted(content::ChromeBlobStorageContext::GetFor(child)),
      permission_root, child->IsOffTheRecord());
  base::ScopedTempDir files;
  ASSERT_TRUE(files.CreateUniqueTempDir());
  const auto path = files.GetPath().AppendASCII("selected.txt");
  ASSERT_TRUE(base::WriteFile(path, "NATIVE_GESTURE_SELECTED_FILE"));
  manager->SetFilePickerResultForTesting(content::PathInfo(path));
  mojo::Remote<blink::mojom::FileSystemAccessManager> remote;
  manager->BindReceiver({frame->GetStorageKey(), url, frame->GetGlobalId()},
                        remote.BindNewPipeAndPassReceiver());

  int input_pipe[2], output_pipe[2];
  ASSERT_EQ(pipe2(input_pipe, O_CLOEXEC), 0);
  base::ScopedFD input_read(input_pipe[0]), input_write(input_pipe[1]);
  ASSERT_EQ(pipe2(output_pipe, O_CLOEXEC), 0);
  base::ScopedFD output_read(output_pipe[0]), output_write(output_pipe[1]);
  base::LaunchOptions launch;
  launch.fds_to_remap = {{input_read.get(), STDIN_FILENO}, {output_write.get(), STDOUT_FILENO}};
  auto process = base::LaunchProcess(std::vector<std::string>{"node",
      tools.AppendASCII("tests").AppendASCII("native_gesture_driver.mjs").value(),
      tools.AppendASCII("profile_user_action.mjs").value()}, launch);
  ASSERT_TRUE(process.IsValid());
  base::ScopedClosureRunner cleanup(base::BindOnce([](base::Process* process) {
    process->Terminate(1, true);
  }, base::Unretained(&process)));
  input_read.reset();
  output_write.reset();

  base::OnceClosure held_notification;
  bool acknowledged = false, done = false;
  int actions = 0;
  for (int step = 0; step < 6 && !done; ++step) {
    const auto line = ReadNativeRequest(output_read.get());
    ASSERT_TRUE(line.has_value());
    const auto request = base::JSONReader::ReadDict(*line, base::JSON_PARSE_RFC);
    ASSERT_TRUE(request.has_value());
    const auto* command = request->FindString("command");
    ASSERT_NE(command, nullptr);
    base::DictValue reply;
    if (*command == "prime") {
      ASSERT_FALSE(held_notification);
      // The renderer-side priming response deliberately overtakes the held
      // native notification. No task timing or sleep chooses this ordering.
      held_notification = base::BindOnce([](content::WeakDocumentPtr document) {
        auto* current = static_cast<content::RenderFrameHostImpl*>(
            document.AsRenderFrameHostIfValid());
        ASSERT_NE(current, nullptr);
        current->UpdateUserActivationState(
            blink::mojom::UserActivationUpdateType::kNotifyActivation,
            blink::mojom::UserActivationNotificationType::kDevTools);
      }, frame->GetWeakDocumentPtr());
      reply.Set("serial", 1);
    } else if (*command == "acknowledge") {
      ASSERT_TRUE(held_notification);
      EXPECT_EQ(actions, 0);
      EXPECT_FALSE(frame->HasTransientUserActivation());
      std::move(held_notification).Run();
      acknowledged = frame->HasTransientUserActivation();
      EXPECT_TRUE(acknowledged);
      reply.Set("native_active", acknowledged);
    } else if (*command == "action") {
      ++actions;
      EXPECT_TRUE(acknowledged) << "Fixture dispatched before native acknowledgement";
      auto options = blink::mojom::FilePickerOptions::New(
          blink::mojom::TypeSpecificFilePickerOptionsUnion::NewOpenFilePickerOptions(
              blink::mojom::OpenFilePickerOptions::New(
                  blink::mojom::AcceptsTypesInfo::New(
                      std::vector<blink::mojom::ChooseFileSystemEntryAcceptsOptionPtr>(), true), false)),
          std::string(), blink::mojom::FilePickerStartInOptionsUnionPtr());
      base::test::TestFuture<blink::mojom::FileSystemAccessErrorPtr,
          std::vector<blink::mojom::FileSystemAccessEntryPtr>> selected;
      remote->ChooseEntries(std::move(options), selected.GetCallback());
      ASSERT_TRUE(selected.Wait());
      const bool ok = selected.Get<0>()->status == blink::mojom::FileSystemAccessStatus::kOk;
      EXPECT_TRUE(ok) << selected.Get<0>()->message;
      EXPECT_EQ(selected.Get<1>().size(), 1u);
      EXPECT_FALSE(frame->HasTransientUserActivation());
      auto* permission = FileSystemAccessPermissionContextFactory::GetForDocument(
          frame->GetWeakDocumentPtr());
      ASSERT_NE(permission, nullptr);
      using Context = content::FileSystemAccessPermissionContext;
      const bool granted = permission->GetReadPermissionGrant(
          frame->GetLastCommittedOrigin(), content::PathInfo(path), Context::HandleType::kFile,
          Context::AccessTrigger::kProgrammaticRead)->GetStatus() ==
          content::FileSystemAccessPermissionGrant::PermissionStatus::GRANTED;
      reply.Set("ok", ok);
      reply.Set("granted", granted);
      reply.Set("message", selected.Get<0>()->message);
    } else if (*command == "done") {
      done = true;
      EXPECT_TRUE(request->FindBool("ok").value_or(false));
      continue;
    } else {
      FAIL() << "Unexpected native fixture command " << *command;
    }
    ASSERT_TRUE(SendNativeReply(input_write.get(), reply));
  }
  EXPECT_TRUE(done);
  EXPECT_EQ(actions, 1);
  EXPECT_TRUE(acknowledged);
  input_write.reset();
  output_read.reset();
  int exit_code = -1;
  ASSERT_TRUE(process.WaitForExitWithTimeout(base::Seconds(15), &exit_code));
  auto disarmed = cleanup.Release();
  disarmed.Reset();
  EXPECT_EQ(exit_code, 0);
  RecordProperty("native_notification_deliberately_held", true);
}

}  // namespace
}  // namespace alloy::site_records
