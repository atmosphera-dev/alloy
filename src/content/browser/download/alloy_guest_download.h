// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_DOWNLOAD_ALLOY_GUEST_DOWNLOAD_H_
#define CONTENT_BROWSER_DOWNLOAD_ALLOY_GUEST_DOWNLOAD_H_
#include <string>
#include "base/functional/callback.h"
#include "content/common/content_export.h"
namespace download { struct DownloadCreateInfo; class DownloadItem; }
namespace content {
class RenderFrameHost;
class WebContents;
CONTENT_EXPORT base::RepeatingCallback<bool()> CaptureAlloyGuestDownloadSource(RenderFrameHost* frame, bool navigation_response = false);
// UI thread: capture original native document and install an app-only stream
// destination. No app path or writable destination descriptor enters the guest.
CONTENT_EXPORT void PrepareAlloyGuestDownload(download::DownloadCreateInfo* info, download::DownloadItem* previous = nullptr);
CONTENT_EXPORT void RememberAlloyGuestDownloadSource(download::DownloadItem* item, const download::DownloadCreateInfo& info);
CONTENT_EXPORT bool ControlAlloyGuestDownload(WebContents* original, const std::string& identity,
    const std::string& operation);
}
#endif
