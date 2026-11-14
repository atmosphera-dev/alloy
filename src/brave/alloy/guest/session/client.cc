// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/client.h"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "brave/alloy/guest/build_identity.h"
#include "brave/alloy/guest/providers.h"
#include "brave/alloy/guest/session/supervisor.h"
#include "brave/alloy/guest/session/extensions.h"
#include "brave/alloy/guest/session/interactions.h"
#include "brave/alloy/guest/stream/stream_client.h"
#include "cef/include/cef_api_hash.h"
#include "cef/include/cef_version.h"
#include "cef/libcef/common/api_version_util.h"
#include "cef/libcef/common/net/url_util.h"
#include "url/gurl.h"

namespace alloy::guest {
bool ParseProtection(const std::string& value, Protection* protection) {
  for (auto item : {Protection::kUnavailable, Protection::kNamedWritersOnly,
                    Protection::kDataWritesDenied, Protection::kReadOnlyView}) {
    if (ProtectionName(item) == value) {
      *protection = item;
      return true;
    }
  }
  return false;
}

std::string ProtectionName(Protection protection) {
  switch (protection) {
    case Protection::kUnavailable: return "unavailable";
    case Protection::kNamedWritersOnly: return "named-writers-only";
    case Protection::kDataWritesDenied: return "data-writes-denied";
    case Protection::kReadOnlyView: return "read-only-view";
  }
  return {};
}

bool ValidateSettings(const Settings& settings) {
  return (settings.mode == "memory" || settings.mode == "stateless") &&
         (settings.network == "keep" ||
          (settings.mode == "stateless" && settings.network == "cut")) &&
         !ProtectionName(settings.required).empty() &&
         (!settings.developmental || settings.required == Protection::kUnavailable) &&
         settings.data_directory.find('\0') == std::string::npos &&
         (settings.data_directory.empty() || settings.data_directory[0] == '/');
}

std::string BuildIdentity() {
  return std::string(CEF_VERSION) + ":" + ALLOY_GUEST_BUILD_ID;
}
std::string ApiIdentity() {
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  return hash ? hash : "";
}

namespace {
class SettingsImpl final : public AlloyGuestSettings {
 public:
  explicit SettingsImpl(Settings settings) : settings_(std::move(settings)) {}
  CefString GetStorageMode() override { return settings_.mode; }
  CefString GetNetworkState() override { return settings_.network; }
  CefString GetRequiredProtection() override { return ProtectionName(settings_.required); }
  bool AllowsDevelopmentalUnavailable() override { return settings_.developmental; }
  CefString GetReadOnlyDataDirectory() override { return settings_.data_directory; }
 private:
  const Settings settings_;
  IMPLEMENT_REFCOUNTING(SettingsImpl);
};

class ProtectionImpl final : public AlloyGuestProtection {
 public:
  explicit ProtectionImpl(std::vector<std::string> fields)
      : fields_(std::move(fields)) {}
  CefString GetFilesystem() override { return fields_[0]; }
  CefString GetProcessCoverage() override { return fields_[1]; }
  CefString GetScratchBacking() override { return fields_[2]; }
  CefString GetSandbox() override { return fields_[3]; }
  CefString GetLimits() override { return fields_[4]; }
 private:
  const std::vector<std::string> fields_;
  IMPLEMENT_REFCOUNTING(ProtectionImpl);
};

class CapabilitiesImpl final : public AlloyGuestCapabilities {
 public:
  explicit CapabilitiesImpl(uint64_t features) : features_(features) {}
  bool IsAvailable(const CefString& feature) override {
    auto name = feature.ToString();
    for (size_t index = 0; index < kFeatureNames.size(); ++index)
      if (name == kFeatureNames[index])
        return (features_ & (uint64_t{1} << index)) != 0;
    return false;
  }
 private:
  const uint64_t features_;
  IMPLEMENT_REFCOUNTING(CapabilitiesImpl);
};

class ViewImpl;
struct Command {
  Message kind = Message::kClose;
  uint64_t view = 0;
  int width = 0;
  int height = 0;
  std::string payload;
  CefRefPtr<AlloyGuestViewHandler> handler;
  Feature feature = Feature::kStream;
  std::string operation;
  ExtensionCompletion completion;
  std::optional<Owner> original_owner;
};

struct ClientCore : public std::enable_shared_from_this<ClientCore> {
  explicit ClientCore(int wake) : wake(wake) {}
  ~ClientCore() { close(wake); }
  bool Enqueue(Command command, const std::string& required_state) {
    std::lock_guard guard(mutex);
    if (closing || state != required_state)
      return false;
    commands.push_back(std::move(command));
    uint64_t one = 1;
    // Nonblocking eventfd; EAGAIN means a wake is already pending.
    ssize_t result;
    do { result = write(wake, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    return true;
  }
  void Close() {
    std::lock_guard guard(mutex);
    if (closing)
      return;
    closing = true;
    state = "CLOSING";
    uint64_t one = 1;
    ssize_t result;
    do { result = write(wake, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
  }
  void Terminate() {
    std::lock_guard guard(mutex);
    if (state == "CLOSED" || terminated)
      return;
    terminated = true;
    closing = true;
    state = "CLOSING";
    // Shutdown, not close: the worker retains FD ownership. This also works
    // while its application callback is running or graceful close is pending.
    if (channel >= 0)
      shutdown(channel, SHUT_RDWR);
    uint64_t one = 1;
    ssize_t result;
    do { result = write(wake, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
  }
  void SetChannel(int value) {
    std::lock_guard guard(mutex);
    channel = value;
    if (channel >= 0 && terminated)
      shutdown(channel, SHUT_RDWR);
  }
  bool IsTerminated() {
    std::lock_guard guard(mutex);
    return terminated;
  }
  std::string GetState() {
    std::lock_guard guard(mutex);
    return state;
  }
  bool IsClosing() {
    std::lock_guard guard(mutex);
    return closing;
  }
  void SetState(const std::string& value) {
    std::lock_guard guard(mutex);
    if (!closing || value == "CLOSED" || value == "FAILED")
      state = value;
  }
  std::deque<Command> TakeCommands() {
    std::lock_guard guard(mutex);
    std::deque<Command> result;
    result.swap(commands);
    return result;
  }
  ExtensionEndpoint Endpoint(uint64_t view) {
    std::weak_ptr<ClientCore> weak = shared_from_this();
    return {
      [weak, view](Feature feature, std::string operation, std::string payload,
                   ExtensionCompletion completion) {
        auto core = weak.lock();
        if (!core) return false;
        Command command;
        command.kind = Message::kExtensionCall;
        command.view = view; command.feature = feature;
        command.operation = std::move(operation);
        command.payload = std::move(payload);
        command.completion = std::move(completion);
        return core->Enqueue(std::move(command), "ACTIVE");
      },
      [weak, view](Feature feature, std::shared_ptr<ExtensionSink> sink) {
        auto core = weak.lock();
        if (!core) return false;
        Owner owner;
        {
          std::lock_guard guard(core->mutex);
          auto found = core->extension_owners.find(view);
          if (core->closing || found == core->extension_owners.end() ||
              core->sinks.contains({view, feature})) return false;
          owner = found->second;
          core->sinks.emplace(std::make_pair(view, feature), sink);
        }
        sink->OwnerChanged(owner);
        return true;
      },
      [weak, view](Feature feature, Owner original, std::string payload) {
        auto core = weak.lock();
        if (!core || original.view != view) return false;
        Command command;
        command.kind = Message::kExtensionCall;
        command.view = view; command.feature = feature;
        command.operation = "@release"; command.original_owner = original;
        command.payload = std::move(payload);
        return core->Enqueue(std::move(command), "ACTIVE");
      },
      [weak, view](Feature feature, Owner original, std::string operation,
                   std::string payload, ExtensionCompletion completion) {
        auto core = weak.lock();
        if (!core || original.view != view) return false;
        Command command;
        command.kind = Message::kExtensionCall;
        command.view = view; command.feature = feature;
        command.operation = std::move(operation); command.original_owner = original;
        command.payload = std::move(payload); command.completion = std::move(completion);
        return core->Enqueue(std::move(command), "ACTIVE");
      },
      [weak, view](Feature feature, Owner original, std::string operation, stream::InteractionData input) {
        auto core = weak.lock();
        return core ? core->Interact(view, feature, original, std::move(operation), std::move(input)) : 0;
      }
    };
  }
  uint64_t Interact(uint64_t view, Feature feature, Owner original,
                     std::string operation, stream::InteractionData input) {
    std::lock_guard guard(mutex);
    auto next = interactions.Next(view);
    if (closing || state != "ACTIVE" || original.view != view ||
        !extension_owners.contains(view) || !next) return 0;
    input.id = *next;
    std::string payload, envelope;
    if (!stream::FitsInteraction(input) || !stream::PackInteraction(input, &payload) ||
        !PackExtension({feature, std::numeric_limits<uint64_t>::max(), operation, payload}, &envelope)) return 0;
    Command command; command.kind = Message::kExtensionCall;
    command.view = view; command.feature = feature; command.operation = std::move(operation);
    command.payload = std::move(payload); command.original_owner = original;
    std::weak_ptr<ClientCore> weak = shared_from_this();
    command.completion = [weak, view, id = input.id](ProviderResult result, uint64_t time, std::string) {
      if (auto core = weak.lock()) core->FinishInteraction(view, id, result, time);
    };
    if (!interactions.Track(feature, original, input)) return 0;
    commands.push_back(std::move(command));
    uint64_t one = 1; ssize_t result;
    do { result = write(wake, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    return input.id;
  }
  bool ReceiveInteraction(Feature feature, Owner original, const std::string& payload) {
    stream::InteractionData value;
    if (!stream::UnpackInteraction(payload, &value) || value.outcome.empty()) return false;
    std::shared_ptr<ExtensionSink> sink;
    {
      std::lock_guard guard(mutex);
      auto observed = interactions.Receive(feature, original, value);
      if (observed == Interactions::Receipt::kMissing) return true;
      if (observed == Interactions::Receipt::kInvalid) return false;
      if (auto receiver = sinks.find({original.view, Feature::kStream}); receiver != sinks.end()) sink = receiver->second;
    }
    if (sink) sink->Event(original, "@interaction", payload);
    return true;
  }
  void FinishInteraction(uint64_t view, uint64_t id, ProviderResult result, uint64_t time) {
    std::optional<Interactions::Action> action;
    std::shared_ptr<ExtensionSink> sink;
    {
      std::lock_guard guard(mutex);
      action = interactions.Complete(view, id);
      if (!action) return;
      if (auto receiver = sinks.find({view, Feature::kStream}); receiver != sinks.end()) sink = receiver->second;
    }
    action->input.dispatch_us = time;
    action->input.outcome = result == ProviderResult::kUnavailable ? "unavailable" :
        result == ProviderResult::kFailed ? "rejected:invalid" : "cancelled-unobserved";
    std::string payload;
    if (sink && stream::PackInteraction(action->input, &payload)) sink->Event(action->owner, "@interaction", std::move(payload));
  }
  void ExtensionOwner(Owner owner) {
    std::vector<std::shared_ptr<ExtensionSink>> notify;
    {
      std::lock_guard guard(mutex);
      extension_owners[owner.view] = owner;
      interactions.BeginView(owner.view);
      for (const auto& [key, sink] : sinks)
        if (key.first == owner.view) notify.push_back(sink);
    }
    for (const auto& sink : notify) sink->OwnerChanged(owner);
  }
  std::shared_ptr<ExtensionSink> Sink(uint64_t view, Feature feature) {
    std::lock_guard guard(mutex);
    auto found = sinks.find({view, feature});
    return found == sinks.end() ? nullptr : found->second;
  }
  void RetireExtensions(uint64_t view) {
    std::vector<std::shared_ptr<ExtensionSink>> retire;
    {
      std::lock_guard guard(mutex);
      extension_owners.erase(view);
      interactions.RetireView(view);
      for (auto it = sinks.begin(); it != sinks.end();) {
        if (it->first.first == view) {
          retire.push_back(it->second);
          retired_sinks.push_back(it->second);
          it = sinks.erase(it);
        } else { ++it; }
      }
    }
    for (const auto& sink : retire) sink->Retire();
  }
  void WaitExtensions() {
    std::vector<std::shared_ptr<ExtensionSink>> retired;
    { std::lock_guard guard(mutex); retired.swap(retired_sinks); }
    for (const auto& sink : retired) sink->Wait();
  }
  const int wake;
  std::mutex mutex;
  std::string state = "STARTING";
  bool closing = false;
  bool terminated = false;
  int channel = -1;
  std::deque<Command> commands;
  std::map<uint64_t, Owner> extension_owners;
  std::map<std::pair<uint64_t, Feature>, std::shared_ptr<ExtensionSink>> sinks;
  std::vector<std::shared_ptr<ExtensionSink>> retired_sinks;
  Interactions interactions;
};

class ViewImpl final : public AlloyGuestView {
 public:
  ViewImpl(uint64_t id, std::weak_ptr<ClientCore> client)
      : id_(id), client_(std::move(client)) {}
  uint64_t GetId() override { return id_; }
  CefRefPtr<AlloyGuestStream> GetStream() override {
    std::lock_guard guard(stream_mutex_);
    if (!stream_) {
      auto client = client_.lock();
      if (!client || closed_) return nullptr;
      std::weak_ptr<ClientCore> weak = client;
      stream_ = stream::MakeClient(client->Endpoint(id_),
          [weak](uint64_t child, CefRefPtr<AlloyGuestViewHandler> handler) {
        auto core = weak.lock();
        if (!core || !handler) return false;
        Command command; command.kind = Message::kExtensionViewOffer;
        command.view = child; command.handler = std::move(handler);
        return core->Enqueue(std::move(command), "ACTIVE");
      });
    }
    return stream_;
  }
  bool Navigate(const CefString& url) override {
    auto client = client_.lock();
    std::string value = url.ToString();
    if (!client || closed_ || value.empty() || value.find('\0') != std::string::npos)
      return false;
    GURL normalized = url_util::MakeGURL(url, /*fixup=*/false);
    std::string packed;
    if (!normalized.is_valid() || !PackFields({normalized.spec()}, &packed))
      return false;
    Command command;
    command.kind = Message::kNavigate;
    command.view = id_;
    command.payload = std::move(packed);
    return client->Enqueue(std::move(command), "ACTIVE");
  }
  void Close() override {
    if (closed_.exchange(true))
      return;
    if (auto client = client_.lock()) {
      client->RetireExtensions(id_);
      Command command;
      command.kind = Message::kCloseView;
      command.view = id_;
      client->Enqueue(std::move(command), "ACTIVE");
    }
  }
  bool closed() const { return closed_; }
  void Retire() { closed_ = true; }
 private:
  const uint64_t id_;
  const std::weak_ptr<ClientCore> client_;
  std::atomic<bool> closed_{false};
  std::mutex stream_mutex_;
  CefRefPtr<AlloyGuestStream> stream_;
  IMPLEMENT_REFCOUNTING(ViewImpl);
};

struct ViewEntry {
  CefRefPtr<ViewImpl> view;
  CefRefPtr<AlloyGuestViewHandler> handler;
  bool ready = false;
  std::optional<std::pair<uint64_t, Feature>> offer_parent;
};

class SessionImpl final : public AlloyGuestSession {
 public:
  SessionImpl(Settings settings, CefRefPtr<AlloyGuestSessionHandler> handler,
              SessionId id, int wake)
      : settings_(std::move(settings)), handler_(std::move(handler)), id_(id),
        core_(std::make_shared<ClientCore>(wake)) {}
  void Start() {
    // Worker ownership ends after final completion. It never joins app UI or
    // calls CEF's app message loop; all application callbacks use this sequence.
    std::thread([self = CefRefPtr<SessionImpl>(this)] { self->Run(); }).detach();
  }
  CefString GetId() override {
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    for (uint8_t byte : id_) {
      value.push_back(hex[byte >> 4]);
      value.push_back(hex[byte & 15]);
    }
    return value;
  }
  CefString GetState() override { return core_->GetState(); }
  bool Accept() override {
    Command command;
    command.kind = Message::kAccept;
    return core_->Enqueue(std::move(command), "PREPARED");
  }
  bool CreateView(int width, int height,
                  CefRefPtr<AlloyGuestViewHandler> handler) override {
    if (width <= 0 || height <= 0 || !handler)
      return false;
    Command command;
    command.kind = Message::kCreateView;
    command.width = width;
    command.height = height;
    command.handler = std::move(handler);
    return core_->Enqueue(std::move(command), "ACTIVE");
  }
  void Close() override { core_->Close(); }
  void Terminate() override { core_->Terminate(); }

 private:
  void StateChanged(const std::string& state) {
    core_->SetState(state);
    handler_->OnStateChanged(this, state);
  }
  void CancelView(uint64_t id, const std::string& reason) {
    auto found = views_.find(id);
    if (found == views_.end())
      return;
    ViewEntry entry = std::move(found->second);
    entry.view->Retire();
    core_->RetireExtensions(id);
    views_.erase(found);
    if (entry.offer_parent)
      if (auto sink = core_->Sink(entry.offer_parent->first, entry.offer_parent->second)) sink->ViewRetired(id);
    if (entry.handler) entry.handler->OnClosed(entry.view, reason);
  }
  bool Dispatch(Envelope envelope, Session* session, std::string* reason) {
    if (envelope.message == Message::kExtensionViewOffer) {
      ExtensionRequest offer;
      std::vector<std::string> fields;
      uint64_t values[4] = {};
      if (!UnpackExtension(envelope.payload, &offer) || offer.id ||
          !UnpackFields(offer.payload, 5, &fields)) return false;
      for (size_t i = 0; i < 4; ++i) if (!ParseUnsigned(fields[i], &values[i])) return false;
      Owner opener{values[0], values[1], values[2], values[3]};
      if (!session->ObserveNativeView(envelope.owner)) return false;
      ViewEntry entry{new ViewImpl(envelope.owner.view, core_), nullptr, false,
                       std::make_pair(opener.view, offer.feature)};
      views_.emplace(envelope.owner.view, std::move(entry));
      core_->ExtensionOwner(envelope.owner);
      auto sink = session->Owns(opener) ? core_->Sink(opener.view, offer.feature) : nullptr;
      if (sink) {
        std::string payload;
        PackFields({std::to_string(envelope.owner.view), std::move(fields[4])}, &payload);
        sink->Event(opener, std::move(offer.operation), std::move(payload));
      } else {
        Command close; close.kind = Message::kCloseView; close.view = envelope.owner.view;
        core_->Enqueue(std::move(close), "ACTIVE");
      }
      return true;
    }
    if (envelope.message == Message::kExtensionReply) {
      ExtensionResponse reply;
      if (!UnpackExtensionReply(envelope.payload, &reply)) return false;
      auto found = pending_extensions_.find(reply.id);
      if (found == pending_extensions_.end()) return true;
      auto pending = std::move(found->second);
      pending_extensions_.erase(found);
      if (pending.owner != envelope.owner || pending.feature != reply.feature) return false;
      if (!session->Owns(pending.owner)) reply.result = ProviderResult::kCancelled;
      if (pending.completion) pending.completion(reply.result, reply.completed_us, std::move(reply.payload));
      return true;
    }
    if (envelope.message == Message::kPrepared) {
      std::vector<std::string> fields;
      ProtectionReport report;
      if (!UnpackFields(envelope.payload, 9, &fields) ||
          !ParseProtection(fields[0], &report.filesystem) ||
          fields[5] != "1" || fields[6] != "1" || fields[7] != "1")
        return false;
      uint64_t features = 0;
      auto parsed = std::from_chars(fields[8].data(), fields[8].data() + fields[8].size(), features);
      if (parsed.ec != std::errc() || parsed.ptr != fields[8].data() + fields[8].size() ||
          (features & ~kKnownFeatures))
        return false;
      report.bootstrap_before_engine = true;
      report.inherited_handles_closed = true;
      report.runtime_supervision = true;
      if (!session->Prepared(report))
        return false;
      StateChanged("PREPARED");
      if (!core_->IsClosing())
        handler_->OnPrepared(this, new ProtectionImpl(std::move(fields)),
                             new CapabilitiesImpl(features));
      return true;
    }
    if (envelope.message == Message::kActive) {
      if (!session->Activate())
        return false;
      StateChanged("ACTIVE");
      return true;
    }
    if (envelope.message == Message::kFailure) {
      *reason = envelope.payload == "incompatible" ? "incompatible" : "failed";
      return false;
    }
    if (envelope.message == Message::kClosed) {
      *reason = "closed";
      return false;
    }
    auto found = views_.find(envelope.owner.view);
    if (found == views_.end())
      return true;  // An already retired original owner cannot be resurrected.
    auto view = found->second.view;
    auto handler = found->second.handler;
    if (envelope.message == Message::kViewClosed) {
      session->RetireView(view->GetId());
      CancelView(view->GetId(), envelope.payload == "failed" ? "failed" : "closed");
      return true;
    }
    if (view->closed())
      return true;
    if (envelope.message == Message::kDocumentStarted ||
        envelope.message == Message::kGenerationChanged) {
      bool observed = envelope.message == Message::kDocumentStarted
          ? session->Observe(envelope.owner) : session->ObserveGeneration(envelope.owner);
      if (observed) core_->ExtensionOwner(envelope.owner);
      return observed;
    }
    if (envelope.message == Message::kCancel) {
      if (handler) handler->OnNavigationComplete(view, envelope.owner.document, -3);
      return true;
    }
    if (!session->Owns(envelope.owner))
      return true;
    if (envelope.message == Message::kExtensionEvent) {
      ExtensionRequest event;
      if (!UnpackExtension(envelope.payload, &event) || event.id) return false;
      if (event.operation == "@interaction")
        return core_->ReceiveInteraction(event.feature, envelope.owner, event.payload);
      if (auto sink = core_->Sink(envelope.owner.view, event.feature))
        sink->Event(envelope.owner, std::move(event.operation), std::move(event.payload));
      return true;
    }
    if (envelope.message == Message::kViewCreated) {
      if (found->second.ready || !handler)
        return false;
      found->second.ready = true;
      handler->OnReady(view);
      return true;
    }
    if (envelope.message == Message::kTitle) {
      if (handler) handler->OnTitleChanged(view, envelope.owner.document, envelope.payload);
      return true;
    }
    if (envelope.message == Message::kNavigationComplete) {
      char* end = nullptr;
      errno = 0;
      long status = strtol(envelope.payload.c_str(), &end, 10);
      if (errno || end == envelope.payload.c_str() || *end ||
          status < std::numeric_limits<int>::min() ||
          status > std::numeric_limits<int>::max())
        return false;
      if (handler) handler->OnNavigationComplete(view, envelope.owner.document,
                                                 static_cast<int>(status));
      return true;
    }
    return envelope.message == Message::kUnavailable || envelope.message == Message::kCancel;
  }

  void Run() {
    Supervisor child;
    Session session;
    Peer peer(id_);
    std::unique_ptr<stream::ResourceLane> resources;
    std::string reason = "failed";
    StateChanged("STARTING");
    bool launched = !core_->IsClosing() && LaunchSupervisor(&child);
    if (launched)
      core_->SetChannel(child.channel);
    if (launched)
      resources = std::make_unique<stream::ResourceLane>(child.resources, id_);
    std::deque<Envelope> outgoing;
    if (launched) {
      Envelope hello;
      hello.message = Message::kHello;
      launched = PackFields({BuildIdentity(), ApiIdentity(), settings_.mode,
                             settings_.network, ProtectionName(settings_.required),
                             settings_.developmental ? "1" : "0", settings_.data_directory,
                             std::to_string(kKnownFeatures)},
                            &hello.payload);
      outgoing.push_back(std::move(hello));
    }
    uint64_t next_request = 1;
    uint64_t next_extension = 1;
    bool running = launched;
    bool closing_started = false;
    std::string close_reason;
    auto cancel_command = [this](Command& command) {
      if (command.completion) command.completion(ProviderResult::kCancelled, 0, {});
      if (command.handler) {
        CefRefPtr<ViewImpl> view = new ViewImpl(command.kind == Message::kExtensionViewOffer ? command.view : 0, core_);
        view->Retire();
        command.handler->OnClosed(view, "cancelled");
      }
    };
    while (running) {
      if (core_->IsTerminated())
        break;
      if (core_->IsClosing() && !closing_started) {
        closing_started = true;
        if (close_reason.empty()) close_reason = "closed";
        bool prepared = session.state() == State::kPrepared || session.state() == State::kActive;
        session.BeginClose();
        StateChanged("CLOSING");
        for (auto& command : core_->TakeCommands()) cancel_command(command);
        while (!views_.empty()) CancelView(views_.begin()->first, "cancelled");
        if (!prepared)
          break;  // Startup abort: no admitted browser/view needs native close.
        // Finish an already-started logical frame, but discard all unsent work.
        // Never replace a partially sent frame with a different message.
        if (peer.HasPendingSend() && !outgoing.empty()) {
          Envelope pending = std::move(outgoing.front());
          outgoing.clear();
          outgoing.push_back(std::move(pending));
        } else {
          outgoing.clear();
        }
        outgoing.push_back({Message::kClose, {}, 0, {}, {}});
      }
      auto commands = core_->TakeCommands();
      while (!commands.empty()) {
        Command command = std::move(commands.front());
        commands.pop_front();
        if (core_->IsClosing()) {
          cancel_command(command);
          continue;
        }
        if (command.kind == Message::kExtensionViewOffer) {
          auto found = views_.find(command.view);
          if (found == views_.end() || !found->second.offer_parent ||
              found->second.handler || found->second.ready || found->second.view->closed()) {
            cancel_command(command);
          } else {
            found->second.handler = std::move(command.handler);
          }
          continue;
        }
        Envelope envelope;
        envelope.message = command.kind;
        if (command.kind == Message::kAccept) {
          if (!session.Accept(settings_.required, settings_.developmental)) {
            close_reason = "refused";
            core_->Close();
            break;
          }
        } else if (command.kind == Message::kCreateView) {
          auto owner = session.CreateView(next_request++);
          if (!owner || !PackFields({std::to_string(command.width),
                                    std::to_string(command.height)}, &envelope.payload)) {
            cancel_command(command);
            running = false;
            break;
          }
          envelope.owner = *owner;
          ViewEntry entry{new ViewImpl(owner->view, core_), command.handler, false};
          views_.emplace(owner->view, std::move(entry));
          core_->ExtensionOwner(*owner);
        } else {
          auto owner = session.Find(command.view);
          if (!owner) {
            if (command.completion) command.completion(ProviderResult::kCancelled, 0, {});
            continue;
          }
          envelope.owner = *owner;
          if (command.kind == Message::kNavigate) {
            auto found = views_.find(command.view);
            if (found == views_.end() || found->second.view->closed())
              continue;
            envelope.owner.request = next_request++;
            envelope.payload = std::move(command.payload);
          } else if (command.kind == Message::kCloseView) {
            session.RetireView(command.view);
          } else if (command.kind == Message::kExtensionCall) {
            if (command.original_owner) envelope.owner = *command.original_owner;
            if (next_extension == std::numeric_limits<uint64_t>::max() ||
                !PackExtension({command.feature, next_extension, command.operation,
                                std::move(command.payload)}, &envelope.payload)) {
              if (command.completion) command.completion(ProviderResult::kFailed, 0, {});
              running = false;
              break;
            }
            pending_extensions_.emplace(next_extension++, PendingExtension{
                envelope.owner, command.feature, std::move(command.completion)});
          }
        }
        outgoing.push_back(std::move(envelope));
      }
      if (!running) {
        for (auto& command : commands) cancel_command(command);
        break;
      }
      if (core_->IsClosing() && !closing_started) {
        for (auto& command : commands) cancel_command(command);
        continue;
      }
      pollfd events[] = {{child.channel, static_cast<short>(POLLIN | (outgoing.empty() ? 0 : POLLOUT)), 0},
                         {core_->wake, POLLIN, 0}, {child.pidfd, POLLIN, 0},
                         {child.resources, POLLIN, 0}};
      int observed;
      do { observed = poll(events, 4, -1); } while (observed < 0 && errno == EINTR);
      if (observed < 0)
        break;
      if (events[1].revents) {
        uint64_t value;
        while (read(core_->wake, &value, sizeof(value)) < 0 && errno == EINTR) {}
      }
      if (core_->IsClosing() && !closing_started)
        continue;
      if (events[0].revents & POLLIN) {
        Envelope envelope;
        if (peer.Receive(child.channel, &envelope)) {
          if (closing_started) {
            if (envelope.message == Message::kClosed || envelope.message == Message::kFailure)
              break;
          } else if (!Dispatch(std::move(envelope), &session, &reason)) {
              break;
          }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          break;
        }
      }
      if ((events[0].revents & POLLOUT) && !outgoing.empty()) {
        if (peer.Send(child.channel, outgoing.front()))
          outgoing.pop_front();
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
          break;
      }
      if (!closing_started && (events[3].revents & POLLIN)) {
        stream::Resource resource;
        if (resources->Receive(&resource)) {
          ExtensionRequest event;
          if (!UnpackExtension(resource.envelope.payload, &event)) break;
          if (auto sink = core_->Sink(resource.envelope.owner.view, event.feature))
            sink->Resource(std::move(resource));
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          break;
        }
      }
      if ((events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) || events[2].revents ||
          (events[3].revents & (POLLERR | POLLHUP | POLLNVAL)))
        break;
    }
    if (!close_reason.empty())
      reason = close_reason;
    else if (core_->IsClosing())
      reason = "closed";
    core_->Close();
    session.BeginClose();
    if (!closing_started)
      StateChanged(reason == "closed" || reason == "refused" ? "CLOSING" : "FAILED");
    for (auto& command : core_->TakeCommands()) {
      cancel_command(command);
    }
    while (!views_.empty())
      CancelView(views_.begin()->first, reason == "failed" ? "failed" : "cancelled");
    for (auto& [id, pending] : pending_extensions_)
      if (pending.completion) pending.completion(ProviderResult::kCancelled, 0, {});
    pending_extensions_.clear();
    // EOF is a close request to the supervisor, which serializes it against
    // registration, terminates/reaps members, and only then exits.
    core_->SetChannel(-1);
    ReapSupervisor(&child);
    core_->WaitExtensions();
    session.Closed();
    core_->SetState("CLOSED");
    handler_->OnClosed(this, reason);
    handler_ = nullptr;
  }

  const Settings settings_;
  CefRefPtr<AlloyGuestSessionHandler> handler_;
  const SessionId id_;
  const std::shared_ptr<ClientCore> core_;
  std::map<uint64_t, ViewEntry> views_;
  struct PendingExtension {
    Owner owner;
    Feature feature;
    ExtensionCompletion completion;
  };
  std::map<uint64_t, PendingExtension> pending_extensions_;
  IMPLEMENT_REFCOUNTING(SessionImpl);
};
}  // namespace
}  // namespace alloy::guest

CefRefPtr<AlloyGuestSettings> AlloyCreateGuestSettings(
    const CefString& storage_mode, const CefString& network_state,
    const CefString& required_protection, bool allow_developmental_unavailable,
    const CefString& read_only_data_directory) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  alloy::guest::Settings settings;
  settings.mode = storage_mode.ToString();
  settings.network = network_state.ToString();
  settings.developmental = allow_developmental_unavailable;
  settings.data_directory = read_only_data_directory.ToString();
  if (!alloy::guest::ParseProtection(required_protection.ToString(), &settings.required) ||
      !alloy::guest::ValidateSettings(settings))
    return nullptr;
  return new alloy::guest::SettingsImpl(std::move(settings));
}

CefRefPtr<AlloyGuestSession> AlloyStartGuestSession(
    CefRefPtr<AlloyGuestSettings> settings,
    CefRefPtr<AlloyGuestSessionHandler> handler) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  if (!settings || !handler)
    return nullptr;
  alloy::guest::Settings copied;
  copied.mode = settings->GetStorageMode().ToString();
  copied.network = settings->GetNetworkState().ToString();
  copied.developmental = settings->AllowsDevelopmentalUnavailable();
  copied.data_directory = settings->GetReadOnlyDataDirectory().ToString();
  if (!alloy::guest::ParseProtection(settings->GetRequiredProtection().ToString(), &copied.required) ||
      !alloy::guest::ValidateSettings(copied))
    return nullptr;
  alloy::guest::SessionId id;
  ssize_t count;
  do { count = getrandom(id.data(), id.size(), 0); } while (count < 0 && errno == EINTR);
  if (count != static_cast<ssize_t>(id.size()))
    return nullptr;
  int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake < 0)
    return nullptr;
  CefRefPtr<alloy::guest::SessionImpl> session =
      new alloy::guest::SessionImpl(std::move(copied), std::move(handler), id, wake);
  session->Start();
  return session;
}
