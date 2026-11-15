// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/native_requests.h"

#include <array>
#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "brave/alloy/guest/session/extensions.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/weak_document_ptr.h"
#include "ui/base/clipboard/clipboard_monitor.h"
#include "brave/alloy/guest/stream/input.h"

namespace alloy::guest::integration {
namespace {
using Operation = content::AlloyGuestIntegration::Operation;
// The pin's async X11 requester/Wayland read path has no read deadline. Reuse
// its Linux clipboard transfer bound, not an invented app setting:
// ui/base/x/selection_owner.cc, kIncrementalTransferTimeout = 10 seconds.
constexpr auto kClipboardReadTimeout = content::AlloyGuestIntegration::kClipboardReadTimeout;
constexpr std::array<const char*, 14> kOperations = {
    "clipboard-read", "clipboard-write", "clipboard-sequence", "menu",
    "drag-source", "fullscreen", "select-files", "file-read", "file-write",
    "file-truncate", "file-close", "download", "clipboard-permission", nullptr};

class Bridge final : public content::AlloyGuestIntegration {
 public:
  using RequestCallback = base::RepeatingCallback<void(
      content::RenderFrameHost*, Operation,
      content::AlloyGuestIntegrationData, Reply)>;
  using Capture = base::RepeatingCallback<base::RepeatingCallback<bool()>(
      content::RenderFrameHost*, bool, base::OnceClosure, int64_t)>;
  using Release = base::RepeatingCallback<void(const std::string&)>;
  using Progress = base::RepeatingCallback<void(const std::string&, const std::string&, int64_t, int64_t)>;
  Bridge(RequestCallback request, Capture capture, Release release, Progress progress)
      : request_(std::move(request)), capture_(std::move(capture)), release_(std::move(release)),
        progress_(std::move(progress)) {}
  void Request(content::RenderFrameHost* original, Operation operation,
                content::AlloyGuestIntegrationData data, Reply reply) override {
    request_.Run(original, operation, std::move(data), std::move(reply));
  }
  base::RepeatingCallback<bool()> CaptureAdmission(content::RenderFrameHost* original,
                                                    bool document_only) override {
    return capture_.Run(original, document_only, {}, 0);
  }
  base::RepeatingCallback<bool()> CaptureLifetime(content::RenderFrameHost* original,
      base::OnceClosure retired) override {
    return capture_.Run(original, true, std::move(retired), 0);
  }
  base::RepeatingCallback<bool()> CaptureDownloadAdmission(content::RenderFrameHost* original,
      int64_t navigation_id) override {
    return capture_.Run(original, true, {}, navigation_id);
  }
  void RetireFile(const std::string& capability) override { release_.Run(capability); }
  void DownloadProgress(const std::string& identity, const std::string& state,
      int64_t received, int64_t total) override {
    if (progress_) progress_.Run(identity, state, received, total);
  }
 private:
  const RequestCallback request_;
  const Capture capture_;
  const Release release_;
  const Progress progress_;
};
}  // namespace

struct NativeRequests::PendingInfo {
  Operation operation;
  base::RepeatingCallback<bool(const base::DictValue&,
                                const std::vector<std::string>&)> validate;
  bool accepted = false;
  bool ui_reply = false;
};

// An integration operation's lifetime, not a permission/site-state owner.
// All adapters share this lease; navigation retires it before a blocked old
// renderer can delay commit and therefore delay WeakDocumentPtr invalidation.
struct NativeRequests::DocumentCapture {
  bool live = true;
  std::vector<content::FrameTreeNodeId> ancestors;
  base::RepeatingCallback<bool()> admitted;
  base::OnceClosure retired;
};

NativeRequests::NativeRequests(Owner owner, Event event, Owns owns,
                                std::function<uint64_t()> clock,
                                std::function<uint64_t()> generation, UiDispatch ui)
    : owner_(owner), event_(std::move(event)), owns_(std::move(owns)),
      clock_(std::move(clock)), generation_(std::move(generation)), ui_(std::move(ui)) {}
NativeRequests::~NativeRequests() { Close(); }

void NativeRequests::Bind(content::WebContents* contents) {
  Observe(contents);
  contents_ = contents->GetWeakPtr();
  content::AlloyGuestIntegration::Install(contents, std::make_unique<Bridge>(
      base::BindRepeating([](base::WeakPtr<NativeRequests> self,
                            content::RenderFrameHost* frame, Operation operation,
                            content::AlloyGuestIntegrationData data,
                            content::AlloyGuestIntegration::Reply reply) {
        if (self) self->Request(frame, operation, std::move(data), std::move(reply));
        else std::move(reply).Run(std::nullopt);
      }, weak_.GetWeakPtr()), base::BindRepeating(
      [](base::WeakPtr<NativeRequests> self, content::RenderFrameHost* frame,
          bool document_only, base::OnceClosure retired, int64_t download_navigation) {
        return self ? self->Capture(frame, document_only, std::move(retired), download_navigation)
                    : base::RepeatingCallback<bool()>();
      }, weak_.GetWeakPtr()), base::BindRepeating(
      [](base::WeakPtr<NativeRequests> self, const std::string& capability) {
        if (!self) return;
        base::DictValue details; details.Set("capability", capability);
        self->Notify("file-retired", std::move(details));
      }, weak_.GetWeakPtr()), base::BindRepeating(
      [](base::WeakPtr<NativeRequests> self, const std::string& identity, const std::string& state,
          int64_t received, int64_t total) {
        if (!self) return;
        base::DictValue data; data.Set("download-id", identity); data.Set("state", state);
        data.Set("received", std::to_string(received)); data.Set("total", std::to_string(total));
        self->Notify("download-state", std::move(data));
      }, weak_.GetWeakPtr())));
}

base::RepeatingCallback<bool()> NativeRequests::Capture(
    content::RenderFrameHost* original, bool document_only, base::OnceClosure retired, int64_t download_navigation) {
  if (!original || !contents_ ||
      original->GetBrowserContext() != contents_->GetBrowserContext() ||
      content::WebContents::FromRenderFrameHost(original)->GetOutermostWebContents() != contents_.get())
    return {};
  const Owner owner = owner_;
  auto capture = std::make_shared<DocumentCapture>();
  capture->retired = std::move(retired);
  for (auto* frame = original; frame; frame = frame->GetParentOrOuterDocument())
    capture->ancestors.push_back(frame->GetFrameTreeNodeId());
  for (const auto& [navigation, frame] : navigating_frames_)
    if (navigation != download_navigation && std::find(capture->ancestors.begin(), capture->ancestors.end(), frame) !=
        capture->ancestors.end()) capture->live = false;
  std::erase_if(document_captures_, [](const auto& capture) { return capture.expired(); });
  document_captures_.push_back(capture);
  capture->admitted = base::BindRepeating([](base::WeakPtr<NativeRequests> self, Owner owner,
      content::WeakDocumentPtr document, base::WeakPtr<base::SupportsUserData> state,
      bool document_only) {
    auto* frame = document.AsRenderFrameHostIfValid();
    return self && self->contents_ && !self->closed_ && self->attached_ && owner.document &&
        self->owner_.view == owner.view && self->owner_.document == owner.document &&
        (document_only || self->owner_ == owner) && self->owns_(self->owner_) &&
        frame && frame->IsActive() &&
        frame->GetBrowserContext() == self->contents_->GetBrowserContext() &&
        content::AlloyStateOwnerForContext(state, frame->GetBrowserContext()) &&
        content::AlloyDocumentStateAdmitted(frame);
  }, weak_.GetWeakPtr(), owner, original->GetWeakDocumentPtr(),
      content::AlloyDocumentStateOwner(original), document_only);
  return base::BindRepeating([](std::shared_ptr<DocumentCapture> capture) {
    return capture->live && capture->admitted.Run();
  }, std::move(capture));
}

void NativeRequests::DidStartNavigation(content::NavigationHandle* navigation) {
  if (closed_ || navigation->IsSameDocument()) return;
  const auto frame = navigation->GetFrameTreeNodeId();
  navigating_frames_.insert_or_assign(navigation->GetNavigationId(), frame);
  for (const auto& weak : document_captures_) {
    auto capture = weak.lock();
    if (capture && std::find(capture->ancestors.begin(), capture->ancestors.end(), frame) !=
                       capture->ancestors.end()) capture->live = false;
  }
  // Retire pending native operations now, rather than waiting for document
  // commit, which can itself be waiting for one of their synchronous replies.
  Sweep();
}

void NativeRequests::DidFinishNavigation(content::NavigationHandle* navigation) {
  navigating_frames_.erase(navigation->GetNavigationId());
}

void NativeRequests::WebContentsDestroyed() { Close(); }

void NativeRequests::SetOwner(Owner owner) {
  if (owner == owner_) return;
  owner_ = owner;
  Sweep();
}

void NativeRequests::Request(content::RenderFrameHost* original,
                             Operation operation,
                             content::AlloyGuestIntegrationData data,
                             content::AlloyGuestIntegration::Reply reply) {
  const size_t kind = static_cast<size_t>(operation);
  if (closed_ || !attached_ || !original || kind >= kOperations.size() - 1 ||
      !content::AlloyDocumentStateAdmitted(original)) {
    std::move(reply).Run(std::nullopt);
    return;
  }
  const Owner owner = owner_;
  const auto native_admission = Capture(original, false);
  data.details.Set("requesting-origin", original->GetLastCommittedOrigin().Serialize());
  data.details.Set("embedding-origin",
                   original->GetMainFrame()->GetLastCommittedOrigin().Serialize());
  if (!data.details.contains("generation"))
    data.details.Set("generation", std::to_string(generation_()));
  auto admitted = [native_admission,
                    adapter = std::move(data.admitted)] {
    return native_admission && native_admission.Run() &&
        (!adapter || adapter.Run());
  };
  std::string metadata, sizes;
  std::vector<std::string> lengths;
  lengths.reserve(data.bytes.size());
  for (const auto& bytes : data.bytes) lengths.push_back(std::to_string(bytes.size()));
  if (!base::JSONWriter::Write(data.details, &metadata) ||
      !PackFields(lengths, &sizes)) {
    std::move(reply).Run(std::nullopt);
    return;
  }
  // A shared ID cell lets cancellation erase pending authority before invoking
  // native callbacks, without retaining the registry or a native document.
  auto request_id = std::make_shared<uint64_t>(0);
  auto info = std::make_shared<PendingInfo>();
  info->operation = operation;
  info->ui_reply = operation == Operation::kMenu || operation == Operation::kDragSource ||
      operation == Operation::kFullscreen || operation == Operation::kClipboardWrite ||
      operation == Operation::kSelectFiles || operation == Operation::kDownload ||
      data.details.FindBool("interaction").value_or(false);
  info->validate = std::move(data.validate_reply);
  auto native_reply = std::make_shared<content::AlloyGuestIntegration::Reply>(
      std::move(reply));
  auto event = event_;
  auto id = requests_.Begin(owner, std::move(admitted),
      [weak = weak_.GetWeakPtr(), native_reply, event, owner, request_id, info]
          (bool allowed, std::string details,
                                              std::vector<std::string> bytes) {
        auto parsed = allowed ? base::JSONReader::ReadDict(
            details, base::JSON_PARSE_RFC) : std::nullopt;
        std::optional<content::AlloyGuestIntegrationData> result;
        if (parsed && (!info->validate || info->validate.Run(*parsed, bytes)))
          result.emplace(std::move(*parsed), std::move(bytes));
        const auto* response_status = result ? result->details.FindString("status") : nullptr;
        info->accepted = result && (!response_status || *response_status == "ok");
        if (weak) weak->pending_.erase(*request_id);
        std::string status;
        PackFields({std::to_string(*request_id), result ? "completed" : "retired"}, &status);
        if (*request_id) event(owner, "request-ended", std::move(status));
        std::move(*native_reply).Run(std::move(result));
      }, std::move(data.bytes));
  *request_id = id;
  if (!id) return;
  pending_.emplace(id, info);
  if (operation == Operation::kClipboardRead ||
      operation == Operation::kClipboardSequence) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce([](base::WeakPtr<NativeRequests> self,
                                     Owner original, uint64_t request) {
          if (self) self->requests_.Finish(original, request, false, {});
        }, weak_.GetWeakPtr(), owner, id), kClipboardReadTimeout);
  }
  std::string payload;
  if (!PackFields({std::to_string(id), kOperations[kind], std::move(metadata),
                    std::to_string(lengths.size()), std::move(sizes)}, &payload)) {
    requests_.Finish(owner, id, false, {});
    return;
  }
  std::string envelope;
  if (!PackExtension({Feature::kIntegration, 0, "request", payload}, &envelope)) {
    requests_.Finish(owner, id, false, {});
    return;
  }
  event_(owner, "request", std::move(payload));
}

void NativeRequests::Call(Owner owner, const std::string& operation,
                           const std::string& payload, ProviderReply reply) {
  if (closed_ || owner != owner_) {
    reply(ProviderResult::kCancelled, {});
    return;
  }
  if (operation == "start") {
    if (attached_ || !payload.empty()) { reply(ProviderResult::kFailed, {}); return; }
    attached_ = true;
    reply(ProviderResult::kOk, {});
    return;
  }
  if (operation == "stop" && payload.empty()) {
    Close();
    reply(ProviderResult::kOk, {});
    return;
  }
  if (!attached_) { reply(ProviderResult::kUnavailable, {}); return; }
  if (!owns_(owner)) { reply(ProviderResult::kCancelled, {}); return; }
  if (operation == "complete-ui" || operation == "ui") {
    stream::InteractionData action;
    if (!stream::UnpackInteraction(payload, &action) || !action.id ||
        action.id <= last_action_ || action.document != owner.document ||
        !action.outcome.empty() || action.dispatch_us) {
      reply(ProviderResult::kFailed, {}); return;
    }
    last_action_ = action.id;
    if (operation == "ui") {
      const auto event = event_;
      const auto dispatch = ui_;
      auto completion = [weak = weak_.GetWeakPtr(), event, clock = clock_, owner,
                           action, reply = std::move(reply)](std::string outcome) mutable {
        if (!weak || weak->closed_ || weak->owner_ != owner) {
          reply(ProviderResult::kCancelled, {}); return;
        }
        action.dispatch_us = clock(); action.outcome = std::move(outcome);
        std::string receipt;
        if (stream::PackInteraction(action, &receipt))
          event(owner, stream::kInteractionOperation, std::move(receipt));
        reply(ProviderResult::kOk, {});
      };
      if (dispatch) dispatch(owner, action, std::move(completion));
      else completion("rejected:invalid");
      return;
    }
    auto details = stream::ParseDetails(action.details);
    uint64_t request_id = 0;
    bool valid = details && details->GetType("request") == VTYPE_STRING &&
        ParseUnsigned(details->GetString("request").ToString(), &request_id) &&
        details->GetType("allow") == VTYPE_BOOL &&
        details->GetType("reply") == VTYPE_STRING;
    auto found = valid ? pending_.find(request_id) : pending_.end();
    auto info = found == pending_.end() ? nullptr : found->second;
    valid = valid && info && info->ui_reply;
    const auto event = event_;
    action.dispatch_us = clock_();
    const bool finished = valid && requests_.Finish(owner, request_id,
        details->GetBool("allow"), details->GetString("reply").ToString());
    action.outcome = finished && info->accepted ? "accepted" : "rejected:denied";
    std::string receipt;
    if (stream::PackInteraction(action, &receipt))
      event(owner, stream::kInteractionOperation, std::move(receipt));
    reply(finished ? ProviderResult::kOk : ProviderResult::kFailed, {});
    return;
  }
  if (operation == "clipboard-changed" && payload.empty()) {
    ui::ClipboardMonitor::GetInstance()->NotifyClipboardDataChanged();
    reply(ProviderResult::kOk, {});
    return;
  }
  std::vector<std::string> fields;
  uint64_t id = 0, index = 0, offset = 0, length = 0;
  bool accepted = false;
  if (operation == "read" && UnpackFields(payload, 4, &fields) &&
      ParseUnsigned(fields[0], &id) && ParseUnsigned(fields[1], &index) &&
      ParseUnsigned(fields[2], &offset) && ParseUnsigned(fields[3], &length) &&
      length <= kMaxControlPayload - 256) {
    auto bytes = requests_.Read(owner, id, index, offset, length);
    reply(bytes ? ProviderResult::kOk : ProviderResult::kFailed,
           bytes ? std::move(*bytes) : std::string());
    return;
  }
  if (operation == "respond" && UnpackFields(payload, 3, &fields) &&
      ParseUnsigned(fields[0], &id) && ParseUnsigned(fields[1], &length) &&
      length <= fields[2].size() / 4) {
    std::vector<std::string> sizes;
    if (UnpackFields(fields[2], static_cast<size_t>(length), &sizes)) {
      std::vector<uint64_t> expected;
      bool valid = true;
      for (const auto& size : sizes) {
        uint64_t value = 0;
        if (!ParseUnsigned(size, &value)) { valid = false; break; }
        expected.push_back(value);
      }
      if (valid) accepted = requests_.Respond(owner, id, std::move(expected));
    }
  } else if (operation == "append" && UnpackFields(payload, 4, &fields) &&
             ParseUnsigned(fields[0], &id) && ParseUnsigned(fields[1], &index) &&
             ParseUnsigned(fields[2], &offset)) {
    accepted = requests_.Append(owner, id, index, offset, std::move(fields[3]));
  } else if (operation == "complete" && UnpackFields(payload, 3, &fields) &&
             ParseUnsigned(fields[0], &id) && (fields[1] == "0" || fields[1] == "1")) {
    accepted = requests_.Finish(owner, id, fields[1] == "1", std::move(fields[2]));
  }
  reply(accepted ? ProviderResult::kOk : ProviderResult::kFailed, {});
}

std::vector<base::OnceClosure> NativeRequests::TakeRetiredCaptures() {
  std::vector<base::OnceClosure> callbacks;
  for (auto it = document_captures_.begin(); it != document_captures_.end();) {
    auto capture = it->lock();
    if (!capture) { it = document_captures_.erase(it); continue; }
    if (!capture->live || !capture->admitted.Run()) {
      capture->live = false;
      if (capture->retired) callbacks.push_back(std::move(capture->retired));
    }
    ++it;
  }
  return callbacks;
}

void NativeRequests::Sweep() {
  auto callbacks = TakeRetiredCaptures();
  requests_.Sweep();
  // Native completion can destroy this owner. No member access after it starts.
  for (auto& callback : callbacks) std::move(callback).Run();
}
void NativeRequests::Notify(std::string operation, base::DictValue details) {
  if (closed_ || !attached_ || !owns_(owner_)) return;
  details.Set("generation", std::to_string(generation_()));
  std::string payload;
  if (base::JSONWriter::Write(details, &payload)) event_(owner_, std::move(operation), std::move(payload));
}
void NativeRequests::Close() {
  if (closed_) return;
  closed_ = true;
  attached_ = false;
  auto callbacks = TakeRetiredCaptures();
  requests_.Close();
  for (auto& callback : callbacks) std::move(callback).Run();
}

}  // namespace alloy::guest::integration
