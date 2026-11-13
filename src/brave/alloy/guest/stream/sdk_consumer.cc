// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Explicit synthetic acceptance consumer of the translated public SDK only.
#include "include/alloy/alloy_guest_session.h"
#include "include/cef_api_hash.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::mutex output_mutex;
std::string Quote(const std::string& text) {
  std::ostringstream output;
  output << '"';
  for (unsigned char c : text) {
    if (c == '\\' || c == '"') output << '\\' << static_cast<char>(c);
    else if (c < 32) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
    else output << static_cast<char>(c);
  }
  output << '"';
  return output.str();
}
void Event(const std::string& event, const std::string& fields = {}) {
  std::lock_guard guard(output_mutex);
  std::cout << "GUEST_STREAM_EVENT {\"event\":" << Quote(event)
            << ",\"thread\":" << std::hash<std::thread::id>{}(std::this_thread::get_id())
            << fields << "}" << std::endl;
}
uint64_t Checksum(CefRefPtr<AlloyGuestFrame> frame) {
  std::vector<uint8_t> pixels(frame->GetByteSize());
  if (frame->ReadPixels(pixels.data(), pixels.size(), 0) != pixels.size()) return 0;
  uint64_t hash = 1469598103934665603ULL;
  for (auto byte : pixels) hash = (hash ^ byte) * 1099511628211ULL;
  return hash;
}
struct Held {
  CefRefPtr<AlloyGuestFrame> frame;
  uint64_t checksum;
};
class Consumer final : public AlloyGuestSessionHandler,
                       public AlloyGuestViewHandler,
                       public AlloyGuestStreamHandler {
 public:
  void Start() {
    auto settings = AlloyCreateGuestSettings("memory", "keep", "unavailable", true, "");
    auto session = AlloyStartGuestSession(settings, this);
    std::lock_guard guard(mutex_);
    if (!session_) session_ = std::move(session);
  }
  void OnPrepared(CefRefPtr<AlloyGuestSession> session,
                    CefRefPtr<AlloyGuestProtection>,
                    CefRefPtr<AlloyGuestCapabilities> capabilities) override {
    { std::lock_guard guard(mutex_); session_ = session; }
    Event("prepared", ",\"stream\":" + std::string(capabilities->IsAvailable("stream") ? "true" : "false"));
    session->Accept();
  }
  void OnStateChanged(CefRefPtr<AlloyGuestSession> session, const CefString& state) override {
    Event("state", ",\"state\":" + Quote(state.ToString()));
    if (state == "ACTIVE") session->CreateView(128, 96, this);
  }
  void OnClosed(CefRefPtr<AlloyGuestSession>, const CefString& reason) override {
    Event("closed", ",\"reason\":" + Quote(reason.ToString()));
    std::lock_guard guard(mutex_);
    closed_ = true; session_ = nullptr; closed_cv_.notify_all();
  }
  void OnReady(CefRefPtr<AlloyGuestView> view) override {
    auto stream = view->GetStream();
    { std::lock_guard guard(mutex_); view_ = view; stream_ = stream; }
    Event("view-ready", ",\"view\":" + std::to_string(view->GetId()));
    bool queued = stream && stream->Start(this, 3, 8 * 1024 * 1024);
    Event("start-queued", ",\"ok\":" + std::string(queued ? "true" : "false"));
    if (queued) stream->Configure(128, 96, 1.0, 30);
  }
  void OnTitleChanged(CefRefPtr<AlloyGuestView>, uint64_t document, const CefString& title) override {
    Event("title", ",\"document\":" + std::to_string(document) + ",\"title\":" + Quote(title.ToString()));
  }
  void OnNavigationComplete(CefRefPtr<AlloyGuestView>, uint64_t document, int status) override {
    Event("load", ",\"document\":" + std::to_string(document) + ",\"status\":" + std::to_string(status));
  }
  void OnClosed(CefRefPtr<AlloyGuestView>, const CefString& reason) override {
    Event("view-closed", ",\"reason\":" + Quote(reason.ToString()));
  }
  void OnConfigured(uint64_t generation, int width, int height, double scale) override {
    Event("configured", ",\"generation\":" + std::to_string(generation) +
          ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
          ",\"scale\":" + std::to_string(scale));
  }
  void OnFrame(CefRefPtr<AlloyGuestFrame> frame) override {
    const auto checksum = Checksum(frame);
    uint8_t first[4] = {}, middle[4] = {}, last[4] = {}, left[4] = {}, right[4] = {};
    frame->ReadPixels(first, 4, 0);
    frame->ReadPixels(middle, 4, frame->GetStride() * (frame->GetHeight() / 2) + 4 * (frame->GetWidth() / 2));
    frame->ReadPixels(last, 4, frame->GetByteSize() - 4);
    frame->ReadPixels(left, 4, frame->GetStride() * (frame->GetHeight() / 2) + 4 * (frame->GetWidth() / 4));
    frame->ReadPixels(right, 4, frame->GetStride() * (frame->GetHeight() / 2) + 4 * (frame->GetWidth() * 3 / 4));
    bool pattern = first[3] == 255 && first[2] == (first[0] * 3) % 256 &&
                   first[1] == (first[0] * 5) % 256;
    for (int i = 0; i < 4; ++i) pattern = pattern && first[i] == middle[i] && first[i] == last[i];
    bool split = left[3] == 255 && right[3] == 255 &&
        left[2] == left[0] * 3 % 256 && left[1] == left[0] * 5 % 256 &&
        right[2] == right[0] * 3 % 256 && right[1] == right[0] * 5 % 256;
    for (int i = 0; i < 4; ++i) split = split && first[i] == left[i] && last[i] == right[i];
    Event("frame", ",\"view\":" + std::to_string(frame->GetViewId()) +
          ",\"document\":" + std::to_string(frame->GetDocumentId()) +
          ",\"generation\":" + std::to_string(frame->GetGeneration()) +
          ",\"surface\":" + std::to_string(frame->GetSurface()) +
          ",\"sequence\":" + std::to_string(frame->GetSequence()) +
          ",\"dropped\":" + std::to_string(frame->GetDroppedCount()) +
          ",\"capture_us\":" + std::to_string(frame->GetCaptureTimeUs()) +
          ",\"width\":" + std::to_string(frame->GetWidth()) +
          ",\"height\":" + std::to_string(frame->GetHeight()) +
          ",\"stride\":" + std::to_string(frame->GetStride()) +
          ",\"bytes\":" + std::to_string(frame->GetByteSize()) +
          ",\"checksum\":" + std::to_string(checksum) +
          ",\"source_frame\":" + std::to_string(first[0]) +
          ",\"left_frame\":" + std::to_string(left[0]) +
          ",\"right_frame\":" + std::to_string(right[0]) +
          ",\"split_pattern\":" + std::string(split ? "true" : "false") +
          ",\"pattern\":" + std::string(pattern ? "true" : "false"));
    std::unique_lock lock(mutex_);
    if (retain_ && frame->GetDocumentId() && (pattern || split)) {
      presentation_.push_back({frame, checksum});
      recorder_.push_back({frame, checksum});
    }
    stall_cv_.wait(lock, [this] { return !stall_; });
  }
  void OnUnavailable() override { Event("unavailable"); }
  void OnClosed() override { Event("stream-closed"); }
  bool Command(const std::string& line) {
    std::istringstream input(line);
    std::string operation;
    input >> operation;
    CefRefPtr<AlloyGuestView> view;
    CefRefPtr<AlloyGuestStream> stream;
    CefRefPtr<AlloyGuestSession> session;
    { std::lock_guard guard(mutex_); view = view_; stream = stream_; session = session_; }
    if (operation == "navigate") {
      std::string url; input >> url;
      Event("navigate-queued", ",\"ok\":" + std::string(view && view->Navigate(url) ? "true" : "false"));
    } else if (operation == "configure") {
      int width = 0, height = 0; double scale = 0; input >> width >> height >> scale;
      Event("configure-queued", ",\"ok\":" + std::string(stream && stream->Configure(width, height, scale, 30) ? "true" : "false"));
    } else if (operation == "retain") {
      std::string value; input >> value;
      { std::lock_guard guard(mutex_); retain_ = value == "on"; }
      Event("retaining", ",\"on\":" + std::string(value == "on" ? "true" : "false"));
    } else if (operation == "release") {
      std::string consumer; input >> consumer;
      std::vector<Held> released;
      {
        std::lock_guard guard(mutex_);
        if (consumer == "presentation" || consumer == "all") released.swap(presentation_);
        if (consumer == "recorder" || consumer == "all") {
          released.insert(released.end(), recorder_.begin(), recorder_.end()); recorder_.clear();
        }
      }
      released.clear();
      Event("released", ",\"consumer\":" + Quote(consumer));
    } else if (operation == "held") {
      std::vector<Held> held;
      { std::lock_guard guard(mutex_); held = presentation_; held.insert(held.end(), recorder_.begin(), recorder_.end()); }
      for (const auto& entry : held)
        Event("held-frame", ",\"generation\":" + std::to_string(entry.frame->GetGeneration()) +
              ",\"sequence\":" + std::to_string(entry.frame->GetSequence()) +
              ",\"expected\":" + std::to_string(entry.checksum) +
              ",\"actual\":" + std::to_string(Checksum(entry.frame)));
      Event("held-complete", ",\"references\":" + std::to_string(held.size()));
    } else if (operation == "stall") {
      std::string value; input >> value;
      { std::lock_guard guard(mutex_); stall_ = value == "on"; stall_cv_.notify_all(); }
      Event("stalling", ",\"on\":" + std::string(value == "on" ? "true" : "false"));
    } else if (operation == "close") {
      if (session) session->Close();
    } else if (operation == "terminate") {
      if (session) session->Terminate();
    } else if (operation == "quit") {
      if (session) session->Terminate();
      std::unique_lock lock(mutex_);
      stall_ = false; stall_cv_.notify_all();
      closed_cv_.wait(lock, [this] { return closed_; });
      lock.unlock();
      Command("release all");
      Event("finished");
      return false;
    } else {
      Event("invalid-command", ",\"command\":" + Quote(line));
    }
    return true;
  }
 private:
  std::mutex mutex_;
  std::condition_variable closed_cv_, stall_cv_;
  bool closed_ = false, retain_ = false, stall_ = false;
  CefRefPtr<AlloyGuestSession> session_;
  CefRefPtr<AlloyGuestView> view_;
  CefRefPtr<AlloyGuestStream> stream_;
  std::vector<Held> presentation_, recorder_;
  IMPLEMENT_REFCOUNTING(Consumer);
};
}
int main() {
  // Select/verify the SDK version without CefInitialize or CefExecuteProcess.
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  if (!hash || strcmp(hash, CEF_API_HASH_PLATFORM)) return 1;
  CefRefPtr<Consumer> consumer = new Consumer;
  consumer->Start();
  std::string line;
  while (std::getline(std::cin, line)) if (!consumer->Command(line)) return 0;
  consumer->Command("quit");
  return 0;
}
