// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILES_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILES_H_
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
#include "content/browser/file_system_access/file_system_access_directory_handle_impl.h"
#include "content/browser/file_system_access/file_system_access_file_handle_impl.h"
#include "content/browser/file_system_access/file_system_chooser.h"
#include "content/browser/web_contents_based_canceller.h"
namespace content {
CONTENT_EXPORT bool UsesAlloyGuestExternalFiles(const storage::FileSystemURL& url);
CONTENT_EXPORT std::unique_ptr<FileSystemAccessFileHandleImpl> CreateAlloyGuestFileHandle(
    FileSystemAccessManagerImpl* manager, const FileSystemAccessManagerImpl::BindingContext& context,
    const storage::FileSystemURL& url, const std::string& name,
    const FileSystemAccessManagerImpl::SharedHandleState& state);
CONTENT_EXPORT std::unique_ptr<FileSystemAccessDirectoryHandleImpl> CreateAlloyGuestDirectoryHandle(
    FileSystemAccessManagerImpl* manager, const FileSystemAccessManagerImpl::BindingContext& context,
    const storage::FileSystemURL& url, const FileSystemAccessManagerImpl::SharedHandleState& state);
CONTENT_EXPORT void ShowAlloyGuestFilePicker(RenderFrameHost* original,
    const FileSystemChooser::Options& options, FileSystemChooser::ResultCallback callback,
    FileSystemChooser::ScopedObjects objects, std::unique_ptr<WebContentsBasedCanceller> canceller);
CONTENT_EXPORT base::ScopedClosureRunner HoldAlloyGuestFileSelection(RenderFrameHost* original,
    storage::FileSystemContext* file_system, const std::vector<PathInfo>& entries);
CONTENT_EXPORT void TruncateAlloyGuestSaveFile(FileSystemAccessManagerImpl* manager,
    const FileSystemAccessManagerImpl::BindingContext& context, const storage::FileSystemURL& url,
    base::OnceCallback<void(base::File::Error)> callback);
}  // namespace content
#endif
