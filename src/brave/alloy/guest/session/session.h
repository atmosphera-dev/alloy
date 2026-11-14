// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_SESSION_H_
#define BRAVE_ALLOY_GUEST_SESSION_SESSION_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "brave/alloy/guest/protocol/session.h"

namespace alloy::guest {

// Navigation belongs to its original view/document/request. A GPU stream epoch
// is independent: recovery must not silently retire a still-live navigation.
inline bool SameNavigation(const Owner& original, const Owner& current) {
  return original.view == current.view && original.document == current.document &&
         original.request == current.request;
}

enum class State { kStarting, kPrepared, kActive, kClosing, kClosed, kFailed };
enum class Protection { kUnavailable, kNamedWritersOnly, kDataWritesDenied,
                        kReadOnlyView };

// Copied actual startup facts, not an environment-marker assertion. M2 supplies
// the enforcement provider; the foundation can only report unavailable.
struct ProtectionReport {
  Protection filesystem = Protection::kUnavailable;
  bool bootstrap_before_engine = false;
  bool inherited_handles_closed = false;
  bool runtime_supervision = false;
  bool ram_scratch = false;
  std::string sandbox;
  std::string limits;
};

// The one session/control owner. Call on its serialized client/host sequence.
// IDs never migrate to a replacement document, view or stream generation.
class Session {
 public:
  State state() const { return state_; }
  bool Prepared(const ProtectionReport& report);
  bool Accept(Protection required, bool allow_developmental_unavailable);
  bool Activate();
  bool BeginClose();
  void Closed();
  void Fail();
  std::optional<Owner> CreateView(uint64_t request = 0);
  // Host-originated views have their own ID namespace; ordinary app creates
  // cannot race or reuse an offered popup's session identity.
  std::optional<Owner> CreateNativeView();
  bool ObserveNativeView(const Owner& owner);
  // Only the browser/native navigation owner issues document identities. The
  // client observes those identities; enqueueing a URL is not a document commit.
  std::optional<Owner> Navigate(uint64_t view, uint64_t request = 0);
  std::optional<Owner> Find(uint64_t view) const;
  bool Observe(const Owner& owner);
  bool ObserveGeneration(const Owner& owner);
  bool RetireView(uint64_t view);
  bool Owns(const Owner& owner) const;
  std::optional<Owner> RestartStream(uint64_t view);

 private:
  void RetireAll();
  State state_ = State::kStarting;
  ProtectionReport report_;
  bool accepted_ = false;
  uint64_t next_view_ = 1;
  uint64_t next_native_view_ = uint64_t{1} << 63;
  uint64_t next_document_ = 1;
  uint64_t next_request_ = uint64_t{1} << 63;
  std::map<uint64_t, Owner> owners_;
};
}  // namespace alloy::guest
#endif  // BRAVE_ALLOY_GUEST_SESSION_SESSION_H_
