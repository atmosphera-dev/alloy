// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/client.h"
#include "brave/alloy/guest/session/supervisor.h"
#include "brave/alloy/guest/providers.h"
#include "brave/alloy/guest/session/control_queue.h"

#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/alloy_guest_runtime.h"
#include "base/strings/string_number_conversions.h"
#include "base/token.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "cef/include/alloy/alloy_request_context.h"
#include "cef/include/cef_client.h"
#include "cef/include/cef_command_line.h"
#include "cef/include/cef_render_handler.h"
#include "cef/libcef/browser/browser_host_base.h"
#include "cef/libcef/common/api_version_util.h"
#include "cef/libcef/common/net/url_util.h"
#include "content/public/browser/alloy_document_identity.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/gpu_data_manager_observer.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/http/http_response_headers.h"
#include "net/base/net_errors.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace alloy::guest {
namespace {
using Emit = std::function<void(Envelope)>;
using BeginDocument = std::function<std::optional<Owner>(uint64_t)>;
using Restart = std::function<std::optional<Owner>()>;

class GpuEpoch final : public content::GpuDataManagerObserver {
 public:
  explicit GpuEpoch(std::function<void()> lost) : lost_(std::move(lost)) {
    content::GpuDataManager::GetInstance()->AddObserver(this);
  }
  ~GpuEpoch() override { content::GpuDataManager::GetInstance()->RemoveObserver(this); }
  void OnGpuProcessCrashed() override { lost_(); }
 private:
  const std::function<void()> lost_;
};

// A view knows its native children and injected callbacks, never its session
// consumer. Native navigation/document owners, not URL matching or CEF title
// arrival, bind each callback to the original document.
class NativeView final : public CefClient,
                         public CefRenderHandler,
                         public CefLifeSpanHandler,
                         public content::WebContentsObserver {
 public:
  NativeView(Owner owner, int width, int height, Emit emit,
             BeginDocument begin_document, Restart restart)
      : owner_(owner), width_(width), height_(height), emit_(std::move(emit)),
        begin_document_(std::move(begin_document)), restart_(std::move(restart)) {}
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
    rect = CefRect(0, 0, width_, height_);
  }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType,
                const RectList&, const void*, int, int) override {
    // No resource transport is advertised in M1. M3 owns copies/leases.
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    browser_ = browser;
    Observe(CefBrowserHostBase::FromBrowser(browser)->GetWebContents());
    emit_({Message::kViewCreated, {}, 0, owner_, {}});
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override {
    Observe(nullptr);
    browser_ = nullptr;
    closed_ = true;
    emit_({Message::kViewClosed, {}, 0, owner_, {}});
  }
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
                      const CefString&, const CefString&, WindowOpenDisposition,
                      bool, const CefPopupFeatures&, CefWindowInfo&,
                      CefRefPtr<CefClient>&, CefBrowserSettings&,
                      CefRefPtr<CefDictionaryValue>&, bool*) override {
    // Popups are an unavailable extension until M3 can preserve and attach them.
    return true;
  }
  void Navigate(uint64_t request, const std::string& url) {
    if (closing_ || !browser_)
      return;
    pending_request_ = request;
    browser_->GetMainFrame()->LoadURL(url);
  }
  void Close() {
    if (closing_)
      return;
    closing_ = true;
    pending_.clear();
    current_token_ = {};
    if (browser_)
      browser_->GetHost()->CloseBrowser(false);
    else
      closed_ = true;
  }
  bool closed() const { return closed_; }
  void StreamLost(bool document_lost) {
    if (closing_)
      return;
    auto fresh = restart_();
    if (!fresh) {
      Close();
      return;
    }
    owner_ = *fresh;
    if (document_lost) {
      current_token_ = {};
      pending_.clear();
    }
    emit_({Message::kGenerationChanged, {}, 0, owner_, {}});
  }
  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus) override {
    StreamLost(true);
  }
  void DidStartNavigation(content::NavigationHandle* navigation) override {
    if (closing_ || !navigation->IsInPrimaryMainFrame() || navigation->IsSameDocument())
      return;
    // The initial blank page has no app navigation owner and cannot supply
    // success for a later requested page.
    if (navigation->GetURL().IsAboutBlank() && !pending_request_ && !owner_.document)
      return;
    auto owner = begin_document_(std::exchange(pending_request_, 0));
    if (!owner) {
      Close();
      return;
    }
    owner_ = *owner;
    current_token_ = {};
    // Explicitly cancel each original pending navigation before admitting the
    // replacement's document event. Do not silently drop old callback owners.
    for (const auto& [id, original] : pending_)
      emit_({Message::kCancel, {}, 0, original, {}});
    pending_.clear();
    pending_.emplace(navigation->GetNavigationId(), owner_);
    emit_({Message::kDocumentStarted, {}, 0, owner_, {}});
  }
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    auto found = pending_.find(navigation->GetNavigationId());
    if (closing_ || found == pending_.end())
      return;
    Owner original = found->second;
    pending_.erase(found);
    if (!SameNavigation(original, owner_))
      return;
    original.generation = owner_.generation;
    if (!navigation->HasCommitted() || navigation->GetNetErrorCode() != net::OK) {
      emit_({Message::kNavigationComplete, {}, 0, original,
              std::to_string(static_cast<int>(navigation->GetNetErrorCode()))});
      return;
    }
    current_token_ = content::AlloyDocumentNoiseIdentity(navigation->GetRenderFrameHost());
    status_ = navigation->GetResponseHeaders()
                  ? navigation->GetResponseHeaders()->response_code() : 0;
  }
  void TitleWasSetForMainFrame(content::RenderFrameHost* frame) override {
    if (Current(frame))
      emit_({Message::kTitle, {}, 0, owner_, web_contents()->GetTitle().empty()
              ? std::string() : CefString(web_contents()->GetTitle()).ToString()});
  }
  void DidFinishLoad(content::RenderFrameHost* frame, const GURL&) override {
    if (Current(frame)) {
      // Read the native page title at completed load as well. A title mutation
      // before DidFinishNavigation must not be lost or attributed to blank.
      emit_({Message::kTitle, {}, 0, owner_, CefString(web_contents()->GetTitle()).ToString()});
      emit_({Message::kNavigationComplete, {}, 0, owner_, std::to_string(status_)});
    }
  }
 private:
  bool Current(content::RenderFrameHost* frame) const {
    return !closing_ && web_contents() && frame && frame->IsInPrimaryMainFrame() &&
           owner_.document && !current_token_.is_zero() &&
           content::AlloyDocumentNoiseIdentity(frame) == current_token_;
  }
  Owner owner_;
  const int width_;
  const int height_;
  const Emit emit_;
  const BeginDocument begin_document_;
  const Restart restart_;
  CefRefPtr<CefBrowser> browser_;
  base::Token current_token_;
  std::map<int64_t, Owner> pending_;
  uint64_t pending_request_ = 0;
  int status_ = 0;
  bool closing_ = false;
  bool closed_ = false;
  IMPLEMENT_REFCOUNTING(NativeView);
};

class ContextReady final : public CefRequestContextHandler {
 public:
  explicit ContextReady(std::shared_ptr<bool> ready) : ready_(std::move(ready)) {}
  void OnRequestContextInitialized(CefRefPtr<CefRequestContext>) override { *ready_ = true; }
 private:
  const std::shared_ptr<bool> ready_;
  IMPLEMENT_REFCOUNTING(ContextReady);
};

class HostApp final : public CefApp, public CefBrowserProcessHandler {
 public:
  HostApp(std::string data, std::shared_ptr<bool> ready)
      : data_(std::move(data)), ready_(std::move(ready)) {}
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnContextInitialized() override { *ready_ = true; }
  void OnBeforeCommandLineProcessing(const CefString& process,
                                      CefRefPtr<CefCommandLine> command) override {
    // Headless Ozone is the real off-screen path. Do not connect to DISPLAY or
    // the user's session compositor, and retain Chromium's sandbox.
    command->AppendSwitchWithValue("ozone-platform", "headless");
    command->AppendSwitchWithValue("use-gl", "angle");
    command->AppendSwitchWithValue("use-angle", "swiftshader");
    command->AppendSwitch("disable-breakpad");
    if (!data_.empty())
      command->AppendSwitchWithValue("alloy-data-dir", data_);
  }
 private:
  const std::string data_;
  const std::shared_ptr<bool> ready_;
  IMPLEMENT_REFCOUNTING(HostApp);
};

int RunBrowser(const CefMainArgs& args) {
  Envelope hello;
  if (!ReceiveEnvelope(ALLOY_GUEST_CONTROL_FD, &hello) ||
      hello.message != Message::kHello || hello.sequence != 1 ||
      hello.owner != Owner{})
    return 125;
  Peer peer(hello.session, 1);
  std::vector<std::string> fields;
  Settings settings;
  if (!UnpackFields(hello.payload, 8, &fields) ||
      fields[0] != BuildIdentity() || fields[1] != ApiIdentity()) {
    peer.Send(ALLOY_GUEST_CONTROL_FD, {Message::kFailure, {}, 0, {}, "incompatible"});
    return 125;
  }
  settings.mode = fields[2];
  settings.network = fields[3];
  settings.developmental = fields[5] == "1";
  settings.data_directory = fields[6];
  uint64_t supported_features = 0;
  if ((fields[5] != "0" && fields[5] != "1") ||
      !ParseProtection(fields[4], &settings.required) || !ValidateSettings(settings) ||
      !base::StringToUint64(fields[7], &supported_features) ||
      (supported_features & ~kKnownFeatures))
    return 125;
  struct alloy_guest_bootstrap_facts facts = {};
  if (pread(ALLOY_GUEST_FACTS_FD, &facts, sizeof(facts), 0) != sizeof(facts) ||
      facts.magic != ALLOY_GUEST_FACTS_MAGIC || facts.process != getpid() ||
      !facts.before_engine || !facts.handles_closed ||
      fcntl(ALLOY_GUEST_LIVENESS_FD, F_GETOWN) != getpid())
    return 125;
  // Foundation scratch is private but not promised RAM. M2 replaces the scratch
  // owner and unavailable protection provider together, before libcef loading.
  base::ScopedTempDir scratch;
  if (!scratch.CreateUniqueTempDir())
    return 125;
  CefSettings cef_settings;
  cef_settings.windowless_rendering_enabled = true;
  cef_settings.log_severity = LOGSEVERITY_DISABLE;
  CefString(&cef_settings.root_cache_path) = scratch.GetPath().value();
  CefString(&cef_settings.browser_subprocess_path) = BundledBootstrap();
  auto global_ready = std::make_shared<bool>(false);
  auto context_ready = std::make_shared<bool>(false);
  CefRefPtr<HostApp> app = new HostApp(settings.data_directory, global_ready);
  if (!CefInitialize(args, cef_settings, app, nullptr))
    return 125;
  CefRefPtr<CefRequestContext> context;
  if (fcntl(ALLOY_GUEST_CONTROL_FD, F_SETFL, O_NONBLOCK)) {
    context = nullptr;
    CefShutdown();
    return 125;
  }
  Session session;
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  Providers providers([&session](const Owner& owner) { return session.Owns(owner); },
      [runner](std::function<void()> task) {
    runner->PostTask(FROM_HERE, base::BindOnce(
        [](std::function<void()> callback) { callback(); }, std::move(task)));
  });
  ProtectionReport report;
  report.bootstrap_before_engine = true;
  report.inherited_handles_closed = true;
  report.runtime_supervision = true;
  ControlQueue outgoing;
  Envelope prepared;
  prepared.message = Message::kPrepared;
  uint64_t features = 0;
  for (size_t index = 0; index < kFeatureNames.size(); ++index)
    if (providers.Available(static_cast<Feature>(index)))
      features |= uint64_t{1} << index;
  features &= supported_features;
  static_assert(kMaxControlPayload >= url::kMaxURLChars + 4);
  PackFields({"unavailable", "static pre-engine browser; registered exec/zygote entries",
              "disk-possible", "Chromium enabled; per-role status not yet observed",
              "Developmental foundation: no write refusal or OS IPC/device containment; "
              "media and native connectors unavailable; private scratch is not verified RAM",
              "1", "1", "1", std::to_string(features)}, &prepared.payload);
  bool prepared_queued = false;
  bool prepared_delivered = false;
  std::map<uint64_t, CefRefPtr<NativeView>> views;
  auto gpu_epoch = std::make_unique<GpuEpoch>([&views] {
    for (auto& [id, view] : views)
      view->StreamLost(false);
  });
  bool failed = false;
  bool closing = false;
  auto emit = [&outgoing, &peer](Envelope envelope) {
    outgoing.Push(std::move(envelope), peer.HasPendingSend());
  };
  auto close = [&] {
    if (closing)
      return;
    closing = true;
    providers.Close();
    session.BeginClose();
    for (auto& [id, view] : views)
      view->Close();
  };
  while (true) {
    // External pumping is scheduled by downstream progress, never by an
    // unbounded producer queue. Control I/O stays live while native work yields.
    outgoing.Pump();
    if (!closing && *global_ready && !context) {
      CefRefPtr<ContextReady> handler = new ContextReady(context_ready);
      context = settings.mode == "memory"
          ? AlloyCreateMemoryRequestContext(handler)
          : AlloyCreateStatelessRequestContext(settings.network == "cut" ? ALLOY_NETWORK_STATE_CUT
                                                                         : ALLOY_NETWORK_STATE_KEEP, handler);
      if (!context) {
        failed = true;
        close();
      }
    }
    if (!closing && *context_ready && !prepared_queued) {
      if (!session.Prepared(report)) {
        failed = true;
        close();
      } else {
        prepared_queued = true;
        outgoing.Push(std::move(prepared));
      }
    }
    for (auto found = views.begin(); found != views.end();) {
      if (found->second->closed())
        found = views.erase(found);
      else
        ++found;
    }
    if (closing && views.empty() && outgoing.empty())
      break;
    pollfd event = {ALLOY_GUEST_CONTROL_FD,
                    static_cast<short>((closing || !outgoing.CanProduce() ? 0 : POLLIN) |
                                        (outgoing.empty() ? 0 : POLLOUT)), 0};
    int observed = poll(&event, 1, 1);
    if (observed < 0 && errno == EINTR)
      continue;
    if (observed < 0 || (event.revents & (POLLHUP | POLLERR | POLLNVAL))) {
      failed = true;
      close();
      continue;
    }
    if ((event.revents & POLLOUT) && !outgoing.empty()) {
      if (peer.Send(ALLOY_GUEST_CONTROL_FD, outgoing.front())) {
        if (outgoing.front().message == Message::kPrepared)
          prepared_delivered = true;
        outgoing.pop_front();
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        failed = true;
        close();
      }
    }
    if (!(event.revents & POLLIN) || closing)
      continue;
    Envelope request;
    if (!peer.Receive(ALLOY_GUEST_CONTROL_FD, &request)) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      failed = true;
      close();
      continue;
    }
    if (request.message == Message::kAccept) {
      if (!prepared_delivered || !session.Accept(settings.required, settings.developmental) ||
          !session.Activate()) {
        failed = true;
        close();
      } else {
        providers.Admit();
        emit({Message::kActive, {}, 0, {}, {}});
      }
    } else if (request.message == Message::kClose) {
      close();
    } else if (session.state() != State::kActive) {
      failed = true;
      close();
    } else if (request.message == Message::kCreateView) {
      std::vector<std::string> size;
      int width = 0, height = 0;
      auto owner = session.CreateView(request.owner.request);
      if (!owner || *owner != request.owner || !UnpackFields(request.payload, 2, &size) ||
          !base::StringToInt(size[0], &width) || !base::StringToInt(size[1], &height) ||
          width <= 0 || height <= 0) {
        failed = true;
        close();
        continue;
      }
      auto begin = [&session, &providers, id = owner->view](uint64_t original_request) {
        if (auto original = session.Find(id)) providers.Cancel(*original);
        return session.Navigate(id, original_request);
      };
      auto restart = [&session, &providers, id = owner->view] {
        if (auto original = session.Find(id)) providers.Cancel(*original);
        return session.RestartStream(id);
      };
      CefRefPtr<NativeView> view = new NativeView(*owner, width, height, emit, begin, restart);
      views.emplace(owner->view, view);
      CefWindowInfo window;
      window.SetAsWindowless(kNullWindowHandle);
      CefBrowserSettings browser_settings;
      if (!CefBrowserHost::CreateBrowserSync(window, view, "", browser_settings, nullptr, context)) {
        session.RetireView(owner->view);
        views.erase(owner->view);
        emit({Message::kViewClosed, {}, 0, *owner, "failed"});
      }
    } else if (request.message == Message::kNavigate) {
      auto found = views.find(request.owner.view);
      auto owner = session.Find(request.owner.view);
      std::vector<std::string> url;
      if (found == views.end() || !owner || owner->document != request.owner.document ||
          owner->generation != request.owner.generation) {
        emit({Message::kCancel, {}, 0, request.owner, {}});
        continue;
      }
      if (!request.owner.request || !UnpackFields(request.payload, 1, &url)) {
        failed = true;
        close();
        continue;
      }
      GURL normalized = url_util::MakeGURL(url[0], /*fixup=*/false);
      if (!normalized.is_valid()) {
        Envelope rejected{Message::kNavigationComplete, {}, 0, *owner,
                          std::to_string(net::ERR_INVALID_URL)};
        emit(std::move(rejected));
        continue;
      }
      found->second->Navigate(request.owner.request, normalized.spec());
    } else if (request.message == Message::kCloseView) {
      auto found = views.find(request.owner.view);
      if (found != views.end()) {
        session.RetireView(request.owner.view);
        found->second->Close();
      }
    } else {
      failed = true;
      close();
    }
  }
  views.clear();
  gpu_epoch.reset();
  context = nullptr;
  CefShutdown();
  // The app's final acknowledgement also waits for supervisor-owned children.
  if (!failed) {
    while (!peer.Send(ALLOY_GUEST_CONTROL_FD, {Message::kClosed, {}, 0, {}, {}})) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        failed = true;
        break;
      }
      pollfd writable = {ALLOY_GUEST_CONTROL_FD, POLLOUT, 0};
      if (poll(&writable, 1, -1) < 0 && errno != EINTR) {
        failed = true;
        break;
      }
    }
  }
  return failed ? 125 : 0;
}
}  // namespace
}  // namespace alloy::guest

int AlloyRunGuestHost(const CefMainArgs& args) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  if (!alloy_guest_is_runtime())
    return 125;
  // No app-side initialization or sandbox changes occur through this entry.
  int child = CefExecuteProcess(args, nullptr, nullptr);
  if (child >= 0)
    return child;
  return alloy::guest::RunBrowser(args);
}
