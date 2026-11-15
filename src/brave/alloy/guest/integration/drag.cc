// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/drag.h"

#include <cstdio>
#include <limits>
#include <utility>
#include "base/functional/bind.h"
#include "brave/alloy/guest/integration/clipboard.h"
#include "brave/alloy/guest/stream/input.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "cef/libcef/browser/frame_host_impl.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/alloy_guest_integration.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "content/browser/file_system_access/alloy_guest_uploads.h"
#include "content/browser/download/alloy_guest_download.h"
#include "content/public/browser/storage_partition.h"
#include "storage/browser/file_system/file_system_context.h"

namespace alloy::guest::integration {
namespace {
bool Integer(CefRefPtr<CefDictionaryValue> value, const char* key, int* result) {
  if (value->GetType(key) != VTYPE_INT) return false;
  *result = value->GetInt(key); return true;
}
bool Mask(int operation) {
  return operation == static_cast<int>(DRAG_OPERATION_EVERY) ||
      (operation >= 0 && !(operation & ~0x3f));
}
class MemoryWriter final : public CefWriteHandler {
 public:
  size_t Write(const void* pointer, size_t size, size_t count) override {
    if (!size || count > (bytes.max_size() - bytes.size()) / size || (!pointer && count)) return 0;
    bytes.append(static_cast<const char*>(pointer), size * count);
    return count;
  }
  int Seek(int64_t offset, int whence) override {
    return offset == 0 && whence == SEEK_END ? 0 : -1;
  }
  int64_t Tell() override { return bytes.size(); }
  int Flush() override { return 0; }
  bool MayBlock() override { return false; }
  std::string bytes;
 private:
  IMPLEMENT_REFCOUNTING(MemoryWriter);
};
}  // namespace

struct Actions::Source {
  CefRefPtr<CefBrowser> browser;
  bool live = true, starting = true;
  void Finish(int x, int y, int operation) {
    if (!live) return;
    live = false;
    if (starting || !browser) return;
    browser->GetHost()->DragSourceEndedAt(x, y,
        static_cast<cef_drag_operations_mask_t>(operation));
    browser->GetHost()->DragSourceSystemDragEnded();
    browser = nullptr;
  }
};

Actions::Actions(Owner owner, Live live, Point point, Notify notify)
    : owner_(owner), live_(std::move(live)), point_(std::move(point)), notify_(std::move(notify)) {}
Actions::~Actions() { Reset(); }
void Actions::Bind(CefRefPtr<CefBrowser> browser) { Reset(); browser_ = std::move(browser); }
void Actions::SetOwner(Owner owner) {
  if (owner == owner_) return;
  Reset(); owner_ = owner;
}
void Actions::Reset() {
  // The native owner may still be awaiting a renderer acknowledgement after a
  // terminal drop was accepted by this protocol. Cancel that work as well.
  if (browser_) browser_->GetHost()->DragTargetDragLeave();
  target_ = false;
  if (source_) source_->Finish(0, 0, DRAG_OPERATION_NONE);
  source_.reset();
}

void Actions::Dispatch(Owner original, const stream::InteractionData& action,
                           std::function<void(std::string)> completion) {
  if (action.kind == "drag-enter-files") {
    if (!browser_ || original != owner_ || target_ || !live_(original, action.generation)) {
      completion("rejected:stale-generation"); return;
    }
    auto* contents = CefBrowserHostBase::FromBrowser(browser_)->GetWebContents();
    auto* frame = contents ? contents->GetPrimaryMainFrame() : nullptr;
    content::AlloyGuestIntegrationData selection;
    selection.details.Set("mode", "drop-files");
    content::AlloyGuestFileCapability::Select(frame, std::move(selection.details), base::BindOnce(
        [](base::WeakPtr<Actions> self, Owner original, stream::InteractionData action,
            std::function<void(std::string)> completion,
            std::optional<content::AlloyGuestFileCapability::Entries> entries) {
          if (!self || !entries || entries->empty() || !self->browser_ || !self->live_(original, action.generation)) {
            completion("rejected:stale-generation"); return;
          }
          auto* contents = CefBrowserHostBase::FromBrowser(self->browser_)->GetWebContents();
          content::SnapshotAlloyGuestFiles(contents ? contents->GetPrimaryMainFrame() : nullptr,
              std::move(*entries), false, base::BindOnce(
              [](base::WeakPtr<Actions> self, Owner original, stream::InteractionData action,
                  std::function<void(std::string)> completion, std::optional<content::AlloyGuestUploadFiles> files) {
                if (!self || !files || !self->browser_ || self->target_ || !self->live_(original, action.generation)) {
                  completion("rejected:stale-generation"); return;
                }
                auto data = stream::ParseDetails(action.details);
                int x = 0, y = 0, modifiers = 0, allowed = 0; CefMouseEvent event;
                if (!data || !Integer(data, "x", &x) || !Integer(data, "y", &y) ||
                    !Integer(data, "modifiers", &modifiers) || !Integer(data, "allowed", &allowed) ||
                    !Mask(allowed) || modifiers < 0 || !self->point_(original.document, action.generation,
                        action.surface, x, y, false, &event)) { completion("rejected:invalid"); return; }
                auto drag = CefDragData::Create();
                for (const auto& file : files->files) drag->AddFile(file.path.AsUTF8Unsafe(), file.display_name.AsUTF8Unsafe());
                event.modifiers = modifiers;
                self->browser_->GetHost()->DragTargetDragEnter(drag, event, static_cast<cef_drag_operations_mask_t>(allowed));
                self->target_ = true; completion("accepted");
              }, self, original, std::move(action), std::move(completion)));
        }, weak_.GetWeakPtr(), original, action, std::move(completion)));
    return;
  }
  if (action.kind != "edit") { completion(DispatchNow(original, action)); return; }
  if (!browser_ || original != owner_ || !live_(original, action.generation)) {
    completion("rejected:stale-generation"); return;
  }
  auto data = stream::ParseDetails(action.details);
  int command = 0;
  if (!data || !Integer(data, "command", &command) || !IsEditCommand(command)) {
    completion("rejected:invalid"); return;
  }
  auto weak = weak_.GetWeakPtr();
  const auto live = live_;
  DispatchEditCaptured(browser_->GetFocusedFrame(), command,
      [weak, live, original, generation = action.generation] {
        return weak && live(original, generation);
      }, [completion = std::move(completion)](bool accepted) {
        completion(accepted ? "accepted" : "rejected:invalid");
      });
}

std::string Actions::DispatchNow(Owner original, const stream::InteractionData& action) {
  if (!browser_ || original != owner_ || !live_(original, action.generation))
    return "rejected:stale-generation";
  auto data = stream::ParseDetails(action.details);
  if (!data) return "rejected:invalid";
  auto host = browser_->GetHost();
  if (action.kind == "download-control") {
    auto* contents = CefBrowserHostBase::FromBrowser(browser_)->GetWebContents();
    return content::ControlAlloyGuestDownload(contents, data->GetString("download-id"), data->GetString("operation"))
        ? "accepted" : "rejected:invalid";
  }
  if (action.kind == "exit-fullscreen") {
    auto* contents = CefBrowserHostBase::FromBrowser(browser_)->GetWebContents();
    if (!contents) return "rejected:invalid";
    contents->ExitFullscreen(true);
    return "accepted";
  }
  if (action.kind == "drag-leave") {
    if (!target_) return "rejected:invalid";
    host->DragTargetDragLeave(); target_ = false; return "accepted";
  }
  int x = 0, y = 0, modifiers = 0, allowed = 0;
  if (!Integer(data, "x", &x) || !Integer(data, "y", &y) ||
      !Integer(data, "modifiers", &modifiers) || modifiers < 0) return "rejected:invalid";
  CefMouseEvent event;
  if (!point_(original.document, action.generation, action.surface, x, y,
                  action.kind != "drag-enter", &event)) return "rejected:invalid";
  event.modifiers = modifiers;
  if (action.kind != "drag-drop" && (!Integer(data, "allowed", &allowed) || !Mask(allowed)))
    return "rejected:invalid";
  if (action.kind == "drag-enter") {
    if (target_ || data->GetType("text") != VTYPE_STRING || data->GetType("html") != VTYPE_STRING ||
        data->GetType("link") != VTYPE_STRING || data->GetType("title") != VTYPE_STRING)
      return "rejected:invalid";
    auto drag = CefDragData::Create();
    drag->SetFragmentText(data->GetString("text")); drag->SetFragmentHtml(data->GetString("html"));
    if (data->GetType("base") == VTYPE_STRING && !data->GetString("base").empty())
      drag->SetFragmentBaseURL(data->GetString("base"));
    const auto link = data->GetString("link");
    if (!link.empty()) {
      if (!GURL(link.ToString()).is_valid()) return "rejected:invalid";
      // CEF's setters create a URL entry even for an empty string. A fragment
      // without a link must remain a fragment, not become an empty-URL drag.
      drag->SetLinkURL(link); drag->SetLinkTitle(data->GetString("title"));
    }
    host->DragTargetDragEnter(drag, event, static_cast<cef_drag_operations_mask_t>(allowed));
    target_ = true;
  } else if (action.kind == "drag-over") {
    if (!target_) return "rejected:invalid";
    host->DragTargetDragOver(event, static_cast<cef_drag_operations_mask_t>(allowed));
  } else if (action.kind == "drag-drop") {
    if (!target_) return "rejected:invalid";
    host->DragTargetDrop(event); target_ = false;
  } else {
    return "rejected:invalid";
  }
  return "accepted";
}

bool Actions::StartDragging(CefRefPtr<CefDragData> drag, int allowed, int x, int y,
                              uint64_t generation) {
  if (!browser_ || !drag || !Mask(allowed) || !live_(owner_, generation)) return false;
  if (source_ && source_->live) { Reset(); return false; }
  auto* contents = CefBrowserHostBase::FromBrowser(browser_)->GetWebContents();
  auto* bridge = content::AlloyGuestIntegration::Get(contents);
  auto* original = bridge ? bridge->NativeDragSource() : nullptr;
  if (!original || !content::AlloyDocumentStateAdmitted(original)) return false;
  auto source = std::make_shared<Source>(); source->browser = browser_;
  source_ = source;
  content::AlloyGuestIntegrationData data;
  std::vector<CefString> paths;
  drag->GetFilePaths(paths);
  if (!paths.empty()) {
    base::ListValue files;
    for (const auto& path : paths) {
      const auto url = original->GetStoragePartition()->GetFileSystemContext()->CreateCrackedFileSystemURL(
          blink::StorageKey(), storage::kFileSystemTypeLocal, base::FilePath::FromUTF8Unsafe(path.ToString()));
      auto cap = content::AlloyGuestFileCapability::FromURL(original->GetBrowserContext(), url);
      if (!cap || !cap->Admitted()) { source_.reset(); return false; }
      files.Append(cap->metadata().capability);
    }
    data.details.Set("file-capabilities", std::move(files));
  }
  data.details.Set("generation", std::to_string(generation));
  data.details.Set("allowed", allowed); data.details.Set("x", x); data.details.Set("y", y);
  data.details.Set("text", drag->GetFragmentText().ToString());
  data.details.Set("html", drag->GetFragmentHtml().ToString());
  data.details.Set("base", drag->GetFragmentBaseURL().ToString());
  data.details.Set("link", drag->GetLinkURL().ToString());
  data.details.Set("title", drag->GetLinkTitle().ToString());
  data.details.Set("file-name", drag->GetFileName().ToString());
  CefRefPtr<MemoryWriter> writer = new MemoryWriter;
  auto stream = CefStreamWriter::CreateForHandler(writer);
  if (drag->GetFileContents(stream) != writer->bytes.size()) { source_.reset(); return false; }
  data.bytes.push_back(std::move(writer->bytes));
  auto image = drag->GetImage();
  int width = 0, height = 0;
  auto png = image ? image->GetAsPNG(1, true, width, height) : nullptr;
  std::string pixels(png ? png->GetSize() : 0, '\0');
  if (png && png->GetData(pixels.data(), pixels.size(), 0) != pixels.size()) {
    source_.reset(); return false;
  }
  data.bytes.push_back(std::move(pixels));
  const auto hotspot = drag->GetImageHotspot();
  data.details.Set("hotspot-x", hotspot.x); data.details.Set("hotspot-y", hotspot.y);
  data.admitted = base::BindRepeating(
      [](std::shared_ptr<Source> source, Live live, Owner owner, uint64_t generation) {
        return source->live && live(owner, generation);
      }, source, live_, owner_, generation);
  data.validate_reply = base::BindRepeating(
      [](int allowed, const base::DictValue& reply, const std::vector<std::string>& bytes) {
        const auto x = reply.FindInt("x"), y = reply.FindInt("y"), operation = reply.FindInt("operation");
        return bytes.empty() && x && y && operation && Mask(*operation) &&
            (*operation == 0 || ((*operation & (*operation - 1)) == 0 && !(*operation & ~allowed)));
      }, allowed);
  bridge->Request(original, content::AlloyGuestIntegration::Operation::kDragSource,
      std::move(data), base::BindOnce([](base::WeakPtr<Actions> self, std::shared_ptr<Source> source,
          std::optional<content::AlloyGuestIntegrationData> reply) {
        source->Finish(reply ? reply->details.FindInt("x").value_or(0) : 0,
            reply ? reply->details.FindInt("y").value_or(0) : 0,
            reply ? reply->details.FindInt("operation").value_or(0) : 0);
        if (self && self->source_ == source) self->source_.reset();
      }, weak_.GetWeakPtr(), source));
  source->starting = false;
  return source->live;
}

void Actions::Cursor(int operation) {
  if (!target_ && (!source_ || !source_->live)) return;
  base::DictValue details;
  details.Set("operation", operation);
  notify_("drag-cursor", std::move(details));
}
}  // namespace alloy::guest::integration
