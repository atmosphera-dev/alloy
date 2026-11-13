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
};

struct ClientCore {
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
  const int wake;
  std::mutex mutex;
  std::string state = "STARTING";
  bool closing = false;
  bool terminated = false;
  int channel = -1;
  std::deque<Command> commands;
};

class ViewImpl final : public AlloyGuestView {
 public:
  ViewImpl(uint64_t id, std::weak_ptr<ClientCore> client)
      : id_(id), client_(std::move(client)) {}
  uint64_t GetId() override { return id_; }
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
  IMPLEMENT_REFCOUNTING(ViewImpl);
};

struct ViewEntry {
  CefRefPtr<ViewImpl> view;
  CefRefPtr<AlloyGuestViewHandler> handler;
  bool ready = false;
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
    views_.erase(found);
    entry.handler->OnClosed(entry.view, reason);
  }
  bool Dispatch(Envelope envelope, Session* session, std::string* reason) {
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
    if (envelope.message == Message::kDocumentStarted)
      return session->Observe(envelope.owner);
    if (envelope.message == Message::kGenerationChanged)
      return session->ObserveGeneration(envelope.owner);
    if (envelope.message == Message::kCancel) {
      handler->OnNavigationComplete(view, envelope.owner.document, -3);
      return true;
    }
    if (!session->Owns(envelope.owner))
      return true;
    if (envelope.message == Message::kViewCreated) {
      if (found->second.ready)
        return false;
      found->second.ready = true;
      handler->OnReady(view);
      return true;
    }
    if (envelope.message == Message::kTitle) {
      handler->OnTitleChanged(view, envelope.owner.document, envelope.payload);
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
      handler->OnNavigationComplete(view, envelope.owner.document,
                                    static_cast<int>(status));
      return true;
    }
    return envelope.message == Message::kUnavailable || envelope.message == Message::kCancel;
  }

  void Run() {
    Supervisor child;
    Session session;
    Peer peer(id_);
    std::string reason = "failed";
    StateChanged("STARTING");
    bool launched = !core_->IsClosing() && LaunchSupervisor(&child);
    if (launched)
      core_->SetChannel(child.channel);
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
    bool running = launched;
    bool closing_started = false;
    std::string close_reason;
    auto cancel_command = [this](Command& command) {
      if (command.handler) {
        CefRefPtr<ViewImpl> view = new ViewImpl(0, core_);
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
        } else {
          auto owner = session.Find(command.view);
          if (!owner)
            continue;
          envelope.owner = *owner;
          if (command.kind == Message::kNavigate) {
            auto found = views_.find(command.view);
            if (found == views_.end() || found->second.view->closed())
              continue;
            envelope.owner.request = next_request++;
            envelope.payload = std::move(command.payload);
          } else if (command.kind == Message::kCloseView) {
            session.RetireView(command.view);
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
                         {core_->wake, POLLIN, 0}, {child.pidfd, POLLIN, 0}};
      int observed;
      do { observed = poll(events, 3, -1); } while (observed < 0 && errno == EINTR);
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
      if ((events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) || events[2].revents)
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
      if (command.handler) {
        CefRefPtr<ViewImpl> view = new ViewImpl(0, core_);
        view->Retire();
        command.handler->OnClosed(view, "cancelled");
      }
    }
    while (!views_.empty())
      CancelView(views_.begin()->first, reason == "failed" ? "failed" : "cancelled");
    // EOF is a close request to the supervisor, which serializes it against
    // registration, terminates/reaps members, and only then exits.
    core_->SetChannel(-1);
    ReapSupervisor(&child);
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
