// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_ALLOY_GUEST_INTEGRATION_H_
#define ALLOY_INCLUDE_ALLOY_GUEST_INTEGRATION_H_
#pragma once
#include "include/cef_base.h"
#include "include/cef_values.h"
#include "include/cef_drag_data.h"
#if CEF_API_ADDED(CEF_EXPERIMENTAL)
class AlloyGuestIntegrationHandler;
class AlloyGuestDataHandler;
class AlloyGuestMenuRequest;
class AlloyGuestDragRequest;
class AlloyGuestFullscreenRequest;
class AlloyGuestFile;
class AlloyGuestFileOperation;
class AlloyGuestFileSelection;

///
/// EXPERIMENTAL: Immutable request-owned bytes. Read is asynchronous and copies
/// at most the requested extent; it never exposes a path or descriptor. A read
/// after original-owner retirement is unqueued; already queued reads complete
/// unsuccessfully before integration OnClosed.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestData : public virtual CefBaseRefCounted {
 public:
  ///
  /// Exact byte size, independent of the control-message packet size.
  ///
  /*--cef()--*/
  virtual uint64_t GetSize() = 0;
  ///
  /// Queue a checked byte range. False means unqueued. A queued read completes
  /// once, including cancellation. No synchronous guest UI wait occurs.
  ///
  /*--cef()--*/
  virtual bool Read(uint64_t offset, uint64_t size,
                    CefRefPtr<AlloyGuestDataHandler> handler) = 0;
};

///
/// EXPERIMENTAL: A data-read completion on the integration callback sequence.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestDataHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Successful empty ranges have success true and an empty data value.
  /// Cancellation or invalid original ownership has success false.
  ///
  /*--cef(optional_param=data)--*/
  virtual void OnRead(bool success, uint64_t offset,
                      CefRefPtr<CefBinaryValue> data) = 0;
};

///
/// EXPERIMENTAL: One native clipboard request under its original document.
/// App replies are copied and asynchronously transferred. Deny and completion
/// are single-use; navigation, close and provider loss revoke held requests.
/// Dropping the last unanswered request reference cancels it. Retain a request
/// while deciding asynchronously, including while reading its write payloads.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestClipboardRequest : public virtual CefBaseRefCounted {
 public:
  ///
  /// Session-local native request identity, not authority by itself.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Original native document's session identity.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// Browser-captured requesting frame origin. This is policy input, never
  /// authority to redirect the reply to a different native document.
  ///
  /*--cef()--*/
  virtual CefString GetRequestingOrigin() = 0;
  ///
  /// Browser-captured outermost document origin for this original request.
  ///
  /*--cef()--*/
  virtual CefString GetEmbeddingOrigin() = 0;
  ///
  /// True for an app read, false for a write or sequence query.
  ///
  /*--cef()--*/
  virtual bool IsRead() = 0;
  ///
  /// True for an atomic multi-format write into the app's clipboard sink.
  ///
  /*--cef()--*/
  virtual bool IsWrite() = 0;
  ///
  /// True for a clipboard sequence query, without requesting content.
  ///
  /*--cef()--*/
  virtual bool IsSequenceQuery() = 0;
  ///
  /// True for a native original-document clipboard permission request.
  ///
  /*--cef()--*/
  virtual bool IsPermissionQuery() = 0;
  ///
  /// Whether the native permission request includes reading clipboard data.
  ///
  /*--cef()--*/
  virtual bool NeedsReadPermission() = 0;
  ///
  /// Native user-gesture state captured by the permission request owner.
  ///
  /*--cef()--*/
  virtual bool HasUserGesture() = 0;
  ///
  /// Zero is ordinary copy/paste; one is the primary selection clipboard.
  ///
  /*--cef()--*/
  virtual int GetBuffer() = 0;
  ///
  /// Read format, or "@types" for format enumeration. Write format names are
  /// individually available through GetFormatName. No OS format is inferred.
  ///
  /*--cef()--*/
  virtual CefString GetRequestedFormat() = 0;
  ///
  /// Number of native write formats in this atomic clipboard transaction.
  ///
  /*--cef()--*/
  virtual size_t GetFormatCount() = 0;
  ///
  /// Selected app files carried by a native clipboard write, never OS paths.
  ///
  /*--cef()--*/
  virtual size_t GetFileCount() = 0;
  ///
  /// Original selected app object at the checked index, or empty.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestFile> GetFile(size_t index) = 0;
  ///
  /// Exact format name at index, including opaque Chromium custom formats.
  /// Standard text uses UTF-8, images image/png, other bytes are opaque.
  ///
  /*--cef()--*/
  virtual CefString GetFormatName(size_t index) = 0;
  ///
  /// Immutable bytes for a native write format, or NULL for an invalid index.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestData> GetData(size_t index) = 0;
  ///
  /// Native copied HTML source URL, never a grant to access that URL or a file.
  ///
  /*--cef()--*/
  virtual CefString GetSourceUrl() = 0;
  ///
  /// Native privacy flags: no-cloud 1, no-history 2, concealed 4. The app owns
  /// its clipboard implementation and retention policy.
  ///
  /*--cef()--*/
  virtual int GetPrivacyFlags() = 0;
  ///
  /// Complete format enumeration with the app's current clipboard format names.
  ///
  /*--cef(optional_param=formats)--*/
  virtual bool CompleteTypes(const std::vector<CefString>& formats) = 0;
  ///
  /// Complete a single format read. HTML fragment offsets count UTF-16 units.
  /// Bytes are copied; caller-owned data may be released after this call.
  ///
  /*--cef(optional_param=data,optional_param=source_url)--*/
  virtual bool CompleteRead(CefRefPtr<CefBinaryValue> data,
                            const CefString& source_url,
                            uint32_t fragment_start,
                            uint32_t fragment_end) = 0;
  ///
  /// Complete the atomic write only after the app sink has accepted all bytes.
  /// A failed sink must be denied, not acknowledged as a successful write.
  ///
  /*--cef()--*/
  virtual bool CompleteWrite(const CefString& source,
                               uint64_t app_capture_time_us) = 0;
  ///
  /// Complete a sequence query with the app's nonzero 128-bit token in Chromium
  /// UnguessableToken hexadecimal form, stable until that clipboard changes.
  /// Native partition salting is applied before the renderer receives it.
  ///
  /*--cef()--*/
  virtual bool CompleteSequence(const CefString& sequence) = 0;
  ///
  /// Reply to a native permission query. The existing native document grant
  /// owner applies the decision; it never becomes a replacement-document grant.
  ///
  /*--cef()--*/
  virtual bool CompletePermission(bool allow) = 0;
  ///
  /// Explicitly refuse this request. Safe after retirement, with no fallback.
  ///
  /*--cef()--*/
  virtual void Deny() = 0;
};

///
/// EXPERIMENTAL: One view's native integrations under the existing session.
/// Attach explicitly before navigation to receive native requests. No app-side
/// CEF initialization or native desktop backend is required.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestIntegration : public virtual CefBaseRefCounted {
 public:
  ///
  /// Attach once. Callbacks run on one serialized integration sequence, separate
  /// from native guest UI and control. Retain request references as needed.
  ///
  /*--cef()--*/
  virtual bool Start(CefRefPtr<AlloyGuestIntegrationHandler> handler) = 0;
  ///
  /// Disconnect the provider and revoke pending native requests and external
  /// file capabilities. Idempotent; this endpoint cannot be restarted. The
  /// final OnClosed callback acknowledges integration retirement.
  ///
  /*--cef()--*/
  virtual void Stop() = 0;
  ///
  /// Notify the native clipboard listener owner after an app clipboard change.
  /// Native focus, permissions and original-document admission still apply.
  ///
  /*--cef()--*/
  virtual bool ClipboardChanged() = 0;
  ///
  /// Queue a native editing command (the CEF MENU_ID editing constants), using
  /// the current native focused frame under this document/geometry owner.
  /// Returns a common interaction ID, or zero when unqueued.
  ///
  /*--cef()--*/
  virtual uint64_t Edit(uint64_t document_id, uint64_t generation, int command_id,
                         const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Enter a target drag with copied text/link/HTML. Raw file paths are never
  /// accepted as guest authority; selected files use the file-capability API.
  /// Coordinates are view DIP. Returns the shared interaction ID or zero.
  ///
  /*--cef()--*/
  virtual uint64_t DragEnter(uint64_t document_id, uint64_t generation,
      CefRefPtr<CefDragData> data, int x, int y, int allowed_operations,
       int modifiers, const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Enter a file drag. The app supplies typed objects through OnFileSelection
  /// with mode drop-files; native RAM snapshots feed the normal drop pipeline.
  /// Wait for the interaction dispatch receipt before continuing the drag.
  ///
  /*--cef()--*/
  virtual uint64_t DragEnterFiles(uint64_t document_id, uint64_t generation,
      int x, int y, int allowed_operations, int modifiers,
      const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Pause, resume or cancel the identified native download under its original
  /// document/view, using operation pause, resume or cancel. Uses the same
  /// ordered interaction feed; zero means unqueued, not a native outcome.
  ///
  /*--cef()--*/
  virtual uint64_t DownloadControl(uint64_t document_id, uint64_t generation,
      const CefString& download_id, const CefString& operation,
      const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Continue a target drag using view-DIP coordinates and native effect mask.
  ///
  /*--cef()--*/
  virtual uint64_t DragOver(uint64_t document_id, uint64_t generation,
      int x, int y, int allowed_operations, int modifiers,
      const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Complete the target drop at the original view/geometry owner.
  ///
  /*--cef()--*/
  virtual uint64_t DragDrop(uint64_t document_id, uint64_t generation,
      int x, int y, int modifiers, const CefString& source,
      uint64_t app_capture_time_us) = 0;
  ///
  /// Leave the target and retire its current drag state.
  ///
  /*--cef()--*/
  virtual uint64_t DragLeave(uint64_t document_id, uint64_t generation,
      const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Request native fullscreen exit through the same live interaction path.
  /// Entry is offered by the original native fullscreen permission request.
  ///
  /*--cef()--*/
  virtual uint64_t ExitFullscreen(uint64_t document_id, uint64_t generation,
      const CefString& source, uint64_t app_capture_time_us) = 0;
};

///
/// EXPERIMENTAL: Native menu model copied before CEF's callback returns. Retain
/// while deciding asynchronously; dropping an unanswered request cancels it.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestMenuRequest : public virtual CefBaseRefCounted {
 public:
  ///
  /// Native pending request ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Original native document ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// Original geometry generation.
  ///
  /*--cef()--*/
  virtual uint64_t GetGeneration() = 0;
  ///
  /// Native menu anchor in view DIP.
  ///
  /*--cef()--*/
  virtual int GetX() = 0;
  ///
  /// Native menu anchor in view DIP.
  ///
  /*--cef()--*/
  virtual int GetY() = 0;
  ///
  /// Whether this is CEF's touch-selection quick menu.
  ///
  /*--cef()--*/
  virtual bool IsQuickMenu() = 0;
  ///
  /// Independent flat model copy. Each dictionary has command, label, type,
  /// enabled, checked, parent (-1 for roots), and native. Native false means
  /// the app owns execution as well as presentation (for example printing).
  /// Quick-menu command IDs are CEF QM_EDITFLAG constants; others are MENU_ID.
  ///
  /*--cef()--*/
  virtual CefRefPtr<CefListValue> GetItems() = 0;
  ///
  /// Select an offered enabled ID. Set handled_by_app when the app performed
  /// the action outside the guest. Results join the common interaction feed.
  ///
  /*--cef()--*/
  virtual bool Select(int command_id, int event_flags, bool handled_by_app,
      const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Dismiss this exact native menu, never a replacement menu.
  ///
  /*--cef()--*/
  virtual bool Cancel(const CefString& source, uint64_t app_capture_time_us) = 0;
};

///
/// EXPERIMENTAL: A native fullscreen request after gesture/Permissions Policy
/// checks but before browser admission or renderer success notification.
/// Retain while deciding asynchronously; dropping an unanswered request denies.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestFullscreenRequest : public virtual CefBaseRefCounted {
 public:
  ///
  /// Pending native request ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Original document ID.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// Original geometry generation.
  ///
  /*--cef()--*/
  virtual uint64_t GetGeneration() = 0;
  ///
  /// Native requesting origin, not a replacement-document authorization.
  ///
  /*--cef()--*/
  virtual CefString GetRequestingOrigin() = 0;
  ///
  /// Complete only after the app has accepted/refused its presentation change.
  /// The native browser keeps the original document and gesture owner.
  ///
  /*--cef()--*/
  virtual bool Decide(bool allow, const CefString& source,
                        uint64_t app_capture_time_us) = 0;
};

///
/// EXPERIMENTAL: One native source drag. Content is copied before the native
/// callback returns; no CEF drag pointer, native file path or image handle crosses.
/// Retain while deciding asynchronously; dropping an unanswered request cancels.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestDragRequest : public virtual CefBaseRefCounted {
 public:
  ///
  /// Selected app files in this source drag, not guest filesystem paths.
  ///
  /*--cef()--*/
  virtual size_t GetFileCount() = 0;
  ///
  /// Selected app file at the checked index, or empty.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestFile> GetFile(size_t index) = 0;
  ///
  /// Native pending request identity.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Original native document identity.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// Original geometry generation.
  ///
  /*--cef()--*/
  virtual uint64_t GetGeneration() = 0;
  ///
  /// CEF native source operation mask, including DRAG_OPERATION_EVERY (-1).
  ///
  /*--cef()--*/
  virtual int GetAllowedOperations() = 0;
  ///
  /// Native CEF StartDragging source location (CEF's screen coordinate contract).
  ///
  /*--cef()--*/
  virtual int GetStartX() = 0;
  ///
  /// Native CEF StartDragging source location.
  ///
  /*--cef()--*/
  virtual int GetStartY() = 0;
  ///
  /// Copied source text fragment.
  ///
  /*--cef()--*/
  virtual CefString GetText() = 0;
  ///
  /// Copied source HTML fragment.
  ///
  /*--cef()--*/
  virtual CefString GetHtml() = 0;
  ///
  /// Copied fragment base URL.
  ///
  /*--cef()--*/
  virtual CefString GetBaseUrl() = 0;
  ///
  /// Copied link URL.
  ///
  /*--cef()--*/
  virtual CefString GetLinkUrl() = 0;
  ///
  /// Copied link title.
  ///
  /*--cef()--*/
  virtual CefString GetLinkTitle() = 0;
  ///
  /// Suggested filename for source file bytes, never a host path or authority.
  ///
  /*--cef()--*/
  virtual CefString GetFileName() = 0;
  ///
  /// Immutable source file data, possibly zero-sized.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestData> GetFileData() = 0;
  ///
  /// Copied PNG drag image; zero-sized when no image is available.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestData> GetImagePng() = 0;
  ///
  /// Native image hotspot in image pixels at scale one.
  ///
  /*--cef()--*/
  virtual int GetHotspotX() = 0;
  ///
  /// Native image hotspot in image pixels at scale one.
  ///
  /*--cef()--*/
  virtual int GetHotspotY() = 0;
  ///
  /// Complete source drag at view-DIP coordinates with one permitted native
  /// effect or DRAG_OPERATION_NONE. Both native source-ended paths run once.
  ///
  /*--cef()--*/
  virtual bool Finish(int x, int y, int operation, const CefString& source,
                         uint64_t app_capture_time_us) = 0;
};

///
/// EXPERIMENTAL: An app-owned selected file, directory or write transaction.
/// GetIdentifier preserves entry identity across translated wrappers/views. All
/// methods run on the integration callback sequence. No app path/FD crosses.
/// A transaction returned for open-write commits only when close succeeds;
/// abort or releasing its last library reference must not commit partial data.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestFile : public virtual CefBaseRefCounted {
 public:
  ///
  /// Stable opaque entry identity allocated by AlloyCreateGuestFileIdentity.
  /// Reuse it for the same entry, not for different files or transactions.
  /// No native path or OS handle is used as this identity.
  ///
  /*--cef()--*/
  virtual CefString GetIdentifier() = 0;
  ///
  /// Canonical parent entry identities, root first, excluding this entry.
  /// These are opaque IDs allocated like GetIdentifier, never app paths. Reuse
  /// the same ancestry when selecting an entry separately or via a directory;
  /// native file/directory locks must protect the same hierarchy in both cases.
  ///
  /*--cef()--*/
  virtual void GetAncestorIdentifiers(std::vector<CefString>& identifiers) = 0;
  ///
  /// Display basename only, never a host path; child names must be distinct.
  ///
  /*--cef()--*/
  virtual CefString GetName() = 0;
  ///
  /// True for a directory, false for a file or file write transaction.
  ///
  /*--cef()--*/
  virtual bool IsDirectory() = 0;
  ///
  /// Whether this app capability permits reading current bytes/entries.
  ///
  /*--cef()--*/
  virtual bool CanRead() = 0;
  ///
  /// Whether the app permits writes, in addition to native permission checks.
  ///
  /*--cef()--*/
  virtual bool CanWrite() = 0;
  ///
  /// Current logical byte length, zero for directories.
  ///
  /*--cef()--*/
  virtual uint64_t GetSize() = 0;
  ///
  /// Last modification time as Unix milliseconds, not a host-local date string.
  ///
  /*--cef()--*/
  virtual int64_t GetModifiedTimeMs() = 0;
  ///
  /// App-supplied MIME type, or empty for native well-known-type inference.
  ///
  /*--cef()--*/
  virtual CefString GetMimeType() = 0;
  ///
  /// Asynchronous native operation. Retain while performing app I/O; dropping
  /// the last unanswered operation denies it. Native revocation wins over a
  /// late completion. The app must not commit a failed or aborted transaction.
  ///
  /*--cef()--*/
  virtual void OnOperation(CefRefPtr<AlloyGuestFileOperation> operation) = 0;
};

///
/// EXPERIMENTAL: One app-file operation, scoped to its original native owner.
/// All extents are checked. Writes use one acknowledged chunk at a time. A
/// successful close means the app's actual sink committed, not merely queued.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestFileOperation : public virtual CefBaseRefCounted {
 public:
  ///
  /// Existing integration request identity, also reported by OnRequestEnded.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Original integration document, not a replacement-document grant.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// stat, read, open-write, write, truncate, close, abort, entries, child-file,
  /// child-directory, child-entry, remove, permission, compare or resolve.
  /// External move/rename retain Chromium's native unsupported-operation refusal.
  ///
  /*--cef()--*/
  virtual CefString GetKind() = 0;
  ///
  /// Byte offset for read/write, otherwise zero.
  ///
  /*--cef()--*/
  virtual uint64_t GetOffset() = 0;
  ///
  /// Maximum read extent, exact write size or requested truncate length.
  ///
  /*--cef()--*/
  virtual uint64_t GetLength() = 0;
  ///
  /// Expected source modification time for a read snapshot, in Unix milliseconds.
  /// Fail a changed source rather than splice different versions into one read.
  ///
  /*--cef()--*/
  virtual int64_t GetExpectedModifiedTimeMs() = 0;
  ///
  /// Child/destination basename, not a host path.
  ///
  /*--cef()--*/
  virtual CefString GetName() = 0;
  ///
  /// Whether a child lookup may create a missing entry.
  ///
  /*--cef()--*/
  virtual bool GetCreate() = 0;
  ///
  /// Whether a remove operation includes directory descendants.
  ///
  /*--cef()--*/
  virtual bool GetRecursive() = 0;
  ///
  /// Whether an open-write transaction starts with current file bytes.
  ///
  /*--cef()--*/
  virtual bool GetKeepExistingData() = 0;
  ///
  /// Whether open-write requests native exclusive rather than siloed mode.
  /// App providers preserve their filesystem's transaction/locking semantics.
  ///
  /*--cef()--*/
  virtual bool GetExclusiveWrite() = 0;
  ///
  /// Whether a permission operation asks to write rather than read.
  /// Native security/gesture checks already precede this app decision.
  ///
  /*--cef()--*/
  virtual bool GetWritePermission() = 0;
  ///
  /// Selected destination for compare/resolve, or empty otherwise.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestFile> GetDestination() = 0;
  ///
  /// Immutable request data for write, empty for other operations.
  ///
  /*--cef()--*/
  virtual CefRefPtr<AlloyGuestData> GetData() = 0;
  ///
  /// Acknowledge a fully performed operation. stat refreshes the target object's
  /// metadata; write acknowledges its entire chunk; close acknowledges commit.
  ///
  /*--cef()--*/
  virtual bool Complete() = 0;
  ///
  /// Complete a read, including a successful empty EOF. Size cannot exceed the
  /// requested length. The native reader checks metadata/ranges again.
  ///
  /*--cef(optional_param=data)--*/
  virtual bool CompleteRead(CefRefPtr<CefBinaryValue> data) = 0;
  ///
  /// Return directory children, a requested child, or an open-write transaction.
  /// Objects gain only this original operation's lifetime, never a new document.
  ///
  /*--cef()--*/
  virtual bool CompleteEntries(const std::vector<CefRefPtr<AlloyGuestFile>>& entries) = 0;
  ///
  /// Return permission/comparison result, or a directory-relative path for resolve.
  /// A false resolve result means the entry is not a descendant.
  ///
  /*--cef()--*/
  virtual bool CompleteRelation(bool related, const std::vector<CefString>& path) = 0;
  ///
  /// Fail with not-found, denied, exists, not-directory, not-file, not-empty,
  /// aborted or failed. No error activates a guest filesystem fallback.
  ///
  /*--cef()--*/
  virtual bool Fail(const CefString& error) = 0;
};

///
/// EXPERIMENTAL: App-owned picker/download selection after native preflight.
/// Retain while deciding. Dropping or denying the request cancels it.
///
/*--cef(source=library,added=experimental)--*/
class AlloyGuestFileSelection : public virtual CefBaseRefCounted {
 public:
  ///
  /// Existing native request identity.
  ///
  /*--cef()--*/
  virtual uint64_t GetId() = 0;
  ///
  /// Original integration document identity.
  ///
  /*--cef()--*/
  virtual uint64_t GetDocumentId() = 0;
  ///
  /// upload, upload-multiple, upload-directory, open, open-multiple, save,
  /// directory, download, clipboard-files or drop-files.
  ///
  /*--cef()--*/
  virtual CefString GetMode() = 0;
  ///
  /// Native original requesting origin.
  ///
  /*--cef()--*/
  virtual CefString GetRequestingOrigin() = 0;
  ///
  /// App presentation text, never a host destination path.
  ///
  /*--cef()--*/
  virtual CefString GetTitle() = 0;
  ///
  /// Suggested basename for save/download, not authority.
  ///
  /*--cef()--*/
  virtual CefString GetSuggestedName() = 0;
  ///
  /// Browser-captured download source URL, empty for ordinary file selection.
  ///
  /*--cef()--*/
  virtual CefString GetSourceUrl() = 0;
  ///
  /// Opaque native download identity used by OnDownloadProgress, empty for pickers.
  ///
  /*--cef()--*/
  virtual CefString GetDownloadId() = 0;
  ///
  /// Number of native accept MIME/extension filters.
  ///
  /*--cef()--*/
  virtual size_t GetAcceptTypeCount() = 0;
  ///
  /// Native accept filter at the checked index.
  ///
  /*--cef()--*/
  virtual CefString GetAcceptType(size_t index) = 0;
  ///
  /// Complete the selected objects through the shared interaction route.
  /// Success is queue admission, not proof of upload or sink completion.
  ///
  /*--cef()--*/
  virtual bool Complete(const std::vector<CefRefPtr<AlloyGuestFile>>& files,
      const CefString& source, uint64_t app_capture_time_us) = 0;
  ///
  /// Cancel the original picker or download without any native UI fallback.
  ///
  /*--cef()--*/
  virtual void Deny() = 0;
};

///
/// EXPERIMENTAL: The app's native-integration provider, with no OS fallback.
///
/*--cef(source=client,added=experimental)--*/
class AlloyGuestIntegrationHandler : public virtual CefBaseRefCounted {
 public:
  ///
  /// Native download state after app stream acknowledgements. States are preparing, active,
  /// paused, complete, cancelled or interrupted. Complete means the app committed bytes
  /// and the native download item reached completion, not just sink admission.
  ///
  /*--cef()--*/
  virtual void OnDownloadProgress(const CefString& download_id, const CefString& state,
      int64_t received_bytes, int64_t total_bytes) = 0;
  ///
  /// App-owned file/directory selection and download destination. No native
  /// path/FD is returned; selected app objects receive asynchronous operations.
  ///
  /*--cef()--*/
  virtual void OnFileSelection(CefRefPtr<AlloyGuestFileSelection> request) = 0;
  ///
  /// App-owned clipboard content and document-specific permission. Missing or
  /// refused replies never activate a guest system clipboard or portal.
  ///
  /*--cef()--*/
  virtual void OnClipboardRequest(CefRefPtr<AlloyGuestClipboardRequest> request) = 0;
  ///
  /// App-owned native menu presentation and action selection.
  ///
  /*--cef()--*/
  virtual void OnMenuRequest(CefRefPtr<AlloyGuestMenuRequest> request) = 0;
  ///
  /// App-owned native drag presentation and final source completion.
  ///
  /*--cef()--*/
  virtual void OnDragRequest(CefRefPtr<AlloyGuestDragRequest> request) = 0;
  ///
  /// Fullscreen permission/presentation before native entry admission.
  ///
  /*--cef()--*/
  virtual void OnFullscreenRequest(CefRefPtr<AlloyGuestFullscreenRequest> request) = 0;
  ///
  /// Actual native fullscreen state, including page/user exit.
  ///
  /*--cef()--*/
  virtual void OnFullscreenChanged(uint64_t document_id, uint64_t generation,
                                       bool fullscreen) = 0;
  ///
  /// Native drag target effect feedback. It is not a new user action.
  ///
  /*--cef()--*/
  virtual void OnDragOperationChanged(int operation, uint64_t generation) = 0;
  ///
  /// A previously announced request ended. Reason is "completed" or "retired".
  /// This precedes final OnClosed; no completed-request history is retained.
  ///
  /*--cef()--*/
  virtual void OnRequestEnded(uint64_t request_id, const CefString& reason) = 0;
  ///
  /// Explicit integration unavailability on the serialized client sequence,
  /// never permission to invoke a desktop service.
  ///
  /*--cef()--*/
  virtual void OnUnavailable() = 0;
  ///
  /// Final integration callback after pending requests and reads retire.
  ///
  /*--cef()--*/
  virtual void OnClosed() = 0;
};
///
/// EXPERIMENTAL: Allocate an opaque file-entry identity without CefInitialize.
/// Retain it with the app object representing that entry; it is not authority.
///
/*--cef(added=experimental)--*/
CefString AlloyCreateGuestFileIdentity();
#endif
#endif
