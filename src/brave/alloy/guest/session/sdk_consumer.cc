// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Boundary fixture using only the translated SDK. Output is synthetic test
// observation, not an Alloy product request/interaction logging facility.
#include "include/alloy/alloy_guest_session.h"
#include "include/alloy/alloy_request_context.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
std::mutex output_mutex;
bool verify_signal_policy = false;
struct sigaction expected_sigchld;
sigset_t expected_mask;
std::thread::id signal_policy_thread;
void ReapFixtureChildren(int) {
  int saved = errno;
  while (waitpid(-1, nullptr, WNOHANG) > 0) {}
  errno = saved;
}
bool FixtureSignalsUnchanged() {
  if (std::this_thread::get_id() != signal_policy_thread) return false;
  struct sigaction current;
  sigset_t mask;
  if (sigaction(SIGCHLD, nullptr, &current) || sigprocmask(SIG_SETMASK, nullptr, &mask) ||
      current.sa_handler != expected_sigchld.sa_handler || current.sa_flags != expected_sigchld.sa_flags)
    return false;
  for (int number = 1; number < NSIG; ++number)
    if (sigismember(&mask, number) != sigismember(&expected_mask, number) ||
        sigismember(&current.sa_mask, number) != sigismember(&expected_sigchld.sa_mask, number)) return false;
  return true;
}
bool DenyCallerKill() {
  // No TSYNC: this fixture changes only the API caller, after custody exists.
  sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_send_signal, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIGKILL, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  sock_fprog program = {6, filter};
  return !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) &&
         !prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}
std::string Quote(const std::string& text) {
  const char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '\\' || c == '"') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 32) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += static_cast<char>(c);
    }
  }
  return result + "\"";
}
void Event(const std::string& name, const std::string& kind,
            const std::string& fields = {}) {
  std::lock_guard guard(output_mutex);
  std::cout << "GUEST_EVENT {\"name\":" << Quote(name) << ",\"event\":" << Quote(kind)
            << ",\"thread\":" << std::hash<std::thread::id>{}(std::this_thread::get_id())
            << fields << "}" << std::endl;
}

class Guest final : public AlloyGuestSessionHandler,
                    public AlloyGuestViewHandler {
 public:
  Guest(std::string name, std::string policy)
      : name_(std::move(name)), policy_(std::move(policy)) {}
  void Attach(CefRefPtr<AlloyGuestSession> session) {
    std::lock_guard guard(mutex_);
    if (!closed_ && !session_)
      session_ = std::move(session);
  }
  void OnPrepared(CefRefPtr<AlloyGuestSession> session,
                    CefRefPtr<AlloyGuestProtection> protection,
                    CefRefPtr<AlloyGuestCapabilities> capabilities) override {
    Attach(session);
    bool unavailable = true;
    for (const char* feature : {"linux_lock", "stream", "media", "integration",
                                "accessibility", "namespace_lock", "capture"})
      unavailable = unavailable && !capabilities->IsAvailable(feature);
    Event(name_, "prepared", ",\"id\":" + Quote(session->GetId().ToString()) +
          ",\"filesystem\":" + Quote(protection->GetFilesystem().ToString()) +
          ",\"coverage\":" + Quote(protection->GetProcessCoverage().ToString()) +
            ",\"scratch\":" + Quote(protection->GetScratchBacking().ToString()) +
           ",\"stream_available\":" + (capabilities->IsAvailable("stream") ? "true" : "false") +
           ",\"sandbox\":" + Quote(protection->GetSandbox().ToString()) +
           ",\"limits\":" + Quote(protection->GetLimits().ToString()) +
           ",\"extensions_unavailable\":" + (unavailable ? "true" : "false"));
    if (policy_ == "refuse")
      session->Close();
    else if (policy_ != "manual")
      Event(name_, "accept-queued", ",\"ok\":" + std::string(session->Accept() ? "true" : "false"));
  }
  void OnStateChanged(CefRefPtr<AlloyGuestSession> session,
                        const CefString& state) override {
    Attach(session);
    Event(name_, "state", ",\"state\":" + Quote(state.ToString()));
    if (state == "STARTING" && policy_ == "terminate-start")
      session->Terminate();
    if (state == "STARTING" && policy_ == "close-start")
      session->Close();
    if (state == "ACTIVE" && policy_ == "queue-close") {
      session->CreateView(640, 480, this);
      session->Close();
    }
  }
  void OnClosed(CefRefPtr<AlloyGuestSession> session, const CefString& reason) override {
    {
      std::lock_guard guard(mutex_);
      session_ = nullptr;
      completed_session_ = std::move(session);
      closed_ = true;
    }
    Event(name_, "closed", ",\"reason\":" + Quote(reason.ToString()));
  }
  void OnReady(CefRefPtr<AlloyGuestView> view) override {
    std::string url;
    {
      std::lock_guard guard(mutex_);
      views_[view->GetId()] = view;
      if (!pending_.empty()) {
        url = std::move(pending_.front());
        pending_.pop_front();
      }
    }
    Event(name_, "view-ready", ",\"view\":" + std::to_string(view->GetId()));
    if (!url.empty())
      Event(name_, "navigate-queued", ",\"ok\":" + std::string(view->Navigate(url) ? "true" : "false"));
  }
  void OnTitleChanged(CefRefPtr<AlloyGuestView> view, uint64_t document,
                        const CefString& title) override {
    Event(name_, "title", ",\"view\":" + std::to_string(view->GetId()) +
          ",\"document\":" + std::to_string(document) +
          ",\"title\":" + Quote(title.ToString()));
    if (policy_ == "stall-title" && title == "STALL_CONSUMER") {
      std::unique_lock lock(stall_mutex_);
      Event(name_, "callback-blocked");
      stall_cv_.wait(lock, [this] { return resumed_; });
      Event(name_, "callback-resumed");
    }
  }
  void OnNavigationComplete(CefRefPtr<AlloyGuestView> view,
                              uint64_t document, int status) override {
    Event(name_, "load", ",\"view\":" + std::to_string(view->GetId()) +
          ",\"document\":" + std::to_string(document) +
          ",\"status\":" + std::to_string(status) +
          ",\"reason\":" + Quote(status == ALLOY_GUEST_NAVIGATION_RENDERER_CRASHED
              ? "renderer-crash" : status == -3 ? "cancelled" : "native"));
  }
  void OnClosed(CefRefPtr<AlloyGuestView> view, const CefString& reason) override {
    // Retain the retired SDK object deliberately so the driver can prove stale
    // calls are refused, not accidentally sent to a new view/document.
    {
      std::lock_guard guard(mutex_);
      views_[view->GetId()] = view;
    }
    Event(name_, "view-closed", ",\"view\":" + std::to_string(view->GetId()) +
          ",\"reason\":" + Quote(reason.ToString()));
  }
  void Accept() {
    CefRefPtr<AlloyGuestSession> session;
    { std::lock_guard guard(mutex_); session = session_; }
    Event(name_, "accept-queued", ",\"ok\":" + std::string(session && session->Accept() ? "true" : "false"));
  }
  void Create(const std::string& url) {
    CefRefPtr<AlloyGuestSession> session;
    {
      std::lock_guard guard(mutex_);
      session = session_;
      pending_.push_back(url);
    }
    bool queued = session && session->CreateView(640, 480, this);
    if (!queued) {
      std::lock_guard guard(mutex_);
      pending_.pop_back();
    }
    Event(name_, "create-queued", ",\"ok\":" + std::string(queued ? "true" : "false"));
  }
  void Navigate(uint64_t id, const std::string& url) {
    CefRefPtr<AlloyGuestView> view;
    {
      std::lock_guard guard(mutex_);
      auto found = views_.find(id);
      if (found != views_.end()) view = found->second;
    }
    Event(name_, "navigate-queued", ",\"ok\":" + std::string(view && view->Navigate(url) ? "true" : "false"));
  }
  void Close() {
    CefRefPtr<AlloyGuestSession> session;
    { std::lock_guard guard(mutex_); session = session_; }
    if (session) session->Close();
    Event(name_, "close-called");
  }
  void Terminate() {
    CefRefPtr<AlloyGuestSession> session;
    {
      std::lock_guard guard(mutex_);
      session = session_ ? session_ : completed_session_;
    }
    if (session) session->Terminate();
    Event(name_, "terminate-called", ",\"state\":" +
          Quote(session ? session->GetState().ToString() : "missing"));
  }
  void Resume() {
    std::lock_guard guard(stall_mutex_);
    resumed_ = true;
    stall_cv_.notify_all();
  }
  bool closed() const { return closed_; }
 private:
  const std::string name_;
  const std::string policy_;
  std::mutex mutex_;
  std::mutex stall_mutex_;
  std::condition_variable stall_cv_;
  bool resumed_ = false;
  CefRefPtr<AlloyGuestSession> session_;
  CefRefPtr<AlloyGuestSession> completed_session_;
  std::deque<std::string> pending_;
  std::map<uint64_t, CefRefPtr<AlloyGuestView>> views_;
  std::atomic<bool> closed_{false};
  IMPLEMENT_REFCOUNTING(Guest);
};

class ContextReady final : public CefRequestContextHandler {
 public:
  explicit ContextReady(std::shared_ptr<bool> ready) : ready_(std::move(ready)) {}
  void OnRequestContextInitialized(CefRefPtr<CefRequestContext>) override { *ready_ = true; }
 private:
  const std::shared_ptr<bool> ready_;
  IMPLEMENT_REFCOUNTING(ContextReady);
};
class App final : public CefApp, public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  void OnContextInitialized() override { ready = true; }
  bool ready = false;
  void OnBeforeCommandLineProcessing(const CefString&,
                                      CefRefPtr<CefCommandLine> command) override {
    command->AppendSwitchWithValue("ozone-platform", "headless");
    command->AppendSwitchWithValue("use-gl", "angle");
    command->AppendSwitchWithValue("use-angle", "swiftshader");
  }
 private:
  IMPLEMENT_REFCOUNTING(App);
};
class EncryptedReady final : public AlloyRequestContextCallback {
 public:
  void OnComplete(alloy_site_settings_result_t result,
                  CefRefPtr<CefRequestContext> context) override {
    context_ = result == ALLOY_SETTINGS_OK ? context : nullptr;
    done_ = true;
  }
  bool done_ = false;
  CefRefPtr<CefRequestContext> context_;
 private:
  IMPLEMENT_REFCOUNTING(EncryptedReady);
};
class Locked final : public AlloyProfileLockCallback {
 public:
  void OnComplete(alloy_site_settings_result_t result) override {
    Event("app", "locked", ",\"ok\":" + std::string(result == ALLOY_SETTINGS_OK ? "true" : "false"));
  }
 private:
  IMPLEMENT_REFCOUNTING(Locked);
};
class AppView final : public CefClient, public CefRenderHandler,
                      public CefDisplayHandler, public CefLifeSpanHandler {
 public:
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
    rect = CefRect(0, 0, 640, 480);
  }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
                 const void*, int, int) override {}
  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
    Event("app", "title", ",\"title\":" + Quote(title.ToString()));
  }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override { closed = true; }
  bool closed = false;
 private:
  IMPLEMENT_REFCOUNTING(AppView);
};
}  // namespace

int main(int argc, char** argv) {
  CefMainArgs args(argc, argv);
  CefRefPtr<App> app = new App;
  int child = CefExecuteProcess(args, app, nullptr);
  if (child >= 0)
    return child;
  if (const char* policy = getenv("ALLOY_FIXTURE_SIGCHLD")) {
    std::string selected(policy);
    if (selected != "default" && selected != "ignored" && selected != "nochildwait" && selected != "reaper")
      return 2;
    struct sigaction action = {};
    action.sa_handler = selected == "ignored" ? SIG_IGN : selected == "reaper" ? ReapFixtureChildren : SIG_DFL;
    action.sa_flags = selected == "nochildwait" ? SA_NOCLDWAIT : 0;
    sigemptyset(&action.sa_mask);
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    if (sigaction(SIGCHLD, &action, nullptr) ||
        sigprocmask(SIG_BLOCK, &blocked, nullptr) ||
        sigaction(SIGCHLD, nullptr, &expected_sigchld) ||
        sigprocmask(SIG_SETMASK, nullptr, &expected_mask)) return 2;
    verify_signal_policy = true;
    signal_policy_thread = std::this_thread::get_id();
  }
  std::vector<int> reserved;
  void* inherited_mapping = MAP_FAILED;
  size_t inherited_mapping_size = 0;
  if (const char* path = getenv("ALLOY_FIXTURE_DISK_CAPABILITY")) {
    int fd = open(path, O_RDWR);  // Deliberately inheritable admission fixture.
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) return 1;
    inherited_mapping_size = st.st_size;
    inherited_mapping = mmap(nullptr, inherited_mapping_size,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (inherited_mapping == MAP_FAILED) return 1;
    reserved.push_back(fd);
    Event("driver", "inherited-capability", ",\"fd\":" + std::to_string(fd));
  }
  if (const char* ceiling = getenv("ALLOY_FIXTURE_FD_CEILING")) {
    int last = -1;
    while (last < std::stoi(ceiling)) {
      last = open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (last < 0) return 1;
      reserved.push_back(last);
    }
    Event("driver", "descriptor-ceiling", ",\"last\":" + std::to_string(last));
  }
  bool encrypted = argc == 3 && std::string(argv[1]) == "--flexible-encrypted";
  bool flexible = encrypted || (argc == 3 && std::string(argv[1]) == "--flexible");
  CefRefPtr<AlloyProfile> app_profile;
  CefRefPtr<CefRequestContext> app_context;
  CefRefPtr<CefBrowser> app_browser;
  CefRefPtr<AppView> app_view;
  if (flexible) {
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_DISABLE;
    CefString(&settings.root_cache_path) = argv[2];
    if (encrypted) {
      std::thread create([&] {
        app_profile = AlloyCreateProfile(std::string(argv[2]) + "-encrypted", "synthetic-M2-password");
      });
      create.join();
      if (!app_profile) return 1;
    }
    if (!CefInitialize(args, settings, app, nullptr))
      return 1;
    while (!app->ready) CefDoMessageLoopWork();
    auto ready = std::make_shared<bool>(false);
    if (encrypted) {
      CefRefPtr<EncryptedReady> callback = new EncryptedReady;
      if (!AlloyCreateEncryptedRequestContext(app_profile, new ContextReady(ready), callback)) return 1;
      while (!callback->done_) CefDoMessageLoopWork();
      app_context = callback->context_;
    } else {
      app_context = AlloyCreateMemoryRequestContext(new ContextReady(ready));
    }
    if (!app_context) return 1;
    while (!*ready) CefDoMessageLoopWork();
    CefWindowInfo window;
    window.SetAsWindowless(kNullWindowHandle);
    app_view = new AppView;
    app_browser = CefBrowserHost::CreateBrowserSync(window, app_view, "",
                                                    CefBrowserSettings(), nullptr, app_context);
    if (!app_browser)
      return 1;
    Event("app", "initialized");
  }
  std::map<std::string, CefRefPtr<Guest>> guests;
  std::set<std::string> signal_checked;
  std::string input;
  bool quitting = false;
  while (true) {
    for (const auto& [name, guest] : guests) {
      if (verify_signal_policy && guest->closed() && signal_checked.insert(name).second)
        Event("driver", "signal-policy-preserved", std::string(",\"ok\":") +
              (FixtureSignalsUnchanged() ? "true" : "false"));
    }
    if (flexible)
      CefDoMessageLoopWork();
    if (quitting) {
      bool closed = !flexible || app_view->closed;
      for (const auto& [name, guest] : guests)
        closed = closed && guest->closed();
      if (closed)
        break;
    }
    pollfd event = {STDIN_FILENO, quitting ? short{0} : short{POLLIN}, 0};
    int available = poll(&event, 1, flexible ? 1 : 10);
    if (available <= 0)
      continue;
    char bytes[4096];
    ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
    if (count <= 0) {
      input += "quit\n";
    } else {
      input.append(bytes, count);
    }
    size_t newline;
    while ((newline = input.find('\n')) != std::string::npos) {
      std::istringstream command(input.substr(0, newline));
      input.erase(0, newline + 1);
      std::string operation, name;
      command >> operation >> name;
      if (operation == "close-denied" || operation == "terminate-denied") {
        if (!DenyCallerKill()) return 2;
        Event("driver", "caller-kill-denied");
        operation.erase(operation.find('-'));
      }
      if (operation == "quit") {
        quitting = true;
        for (auto& [id, guest] : guests) guest->Close();
        if (app_browser) app_browser->GetHost()->CloseBrowser(true);
      } else if (operation == "app" && app_browser) {
        app_browser->GetMainFrame()->LoadURL(name);
      } else if (operation == "app-lock" && app_profile) {
        if (!app_profile->LockAsync(new Locked)) return 1;
      } else if (operation == "start") {
        std::string policy, mode, required, developmental, data;
        command >> policy >> mode >> required >> developmental >> data;
        if (guests.contains(name)) return 2;
        CefRefPtr<Guest> guest = new Guest(name, policy);
        auto settings = AlloyCreateGuestSettings(mode, "keep", required,
                                                   developmental == "1", data == "-" ? "" : data);
        auto session = settings ? AlloyStartGuestSession(settings, guest) : nullptr;
        if (!session) {
          Event(name, "start-failed");
          return 1;
        }
        guest->Attach(session);
        guests.emplace(name, guest);
      } else {
        auto found = guests.find(name);
        if (found == guests.end()) return 2;
        if (operation == "accept") found->second->Accept();
        else if (operation == "close") found->second->Close();
        else if (operation == "terminate") found->second->Terminate();
        else if (operation == "resume") found->second->Resume();
        else if (operation == "create") {
          std::string url;
          command >> url;
          found->second->Create(url == "-" ? "" : url);
        } else if (operation == "navigate") {
          uint64_t view;
          std::string url;
          command >> view >> url;
          found->second->Navigate(view, url);
        } else return 2;
      }
    }
  }
  guests.clear();
  if (flexible) {
    app_browser = nullptr;
    app_context = nullptr;
    app_view = nullptr;
    CefShutdown();
  }
  Event("driver", "finished");
  app_profile = nullptr;
  if (inherited_mapping != MAP_FAILED) munmap(inherited_mapping, inherited_mapping_size);
  for (int descriptor : reserved) close(descriptor);
  return 0;
}
