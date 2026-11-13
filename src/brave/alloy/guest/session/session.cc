// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/session.h"

#include <limits>

namespace alloy::guest {
namespace {
bool Next(uint64_t value) {
  return value != 0 && value != std::numeric_limits<uint64_t>::max();
}
}  // namespace

bool Session::Prepared(const ProtectionReport& report) {
  if (state_ != State::kStarting || !report.bootstrap_before_engine ||
      !report.inherited_handles_closed || !report.runtime_supervision)
    return false;
  report_ = report;
  state_ = State::kPrepared;
  return true;
}

bool Session::Accept(Protection required, bool allow_developmental_unavailable) {
  if (state_ != State::kPrepared || accepted_ || required < Protection::kUnavailable ||
      required > Protection::kReadOnlyView ||
      (report_.filesystem == Protection::kUnavailable &&
       !allow_developmental_unavailable) || report_.filesystem < required)
    return false;
  accepted_ = true;
  return true;
}

bool Session::Activate() {
  if (state_ != State::kPrepared || !accepted_)
    return false;
  state_ = State::kActive;
  return true;
}

void Session::RetireAll() {
  owners_.clear();
  accepted_ = false;
}

bool Session::BeginClose() {
  if (state_ == State::kClosing || state_ == State::kClosed ||
      state_ == State::kFailed)
    return false;
  state_ = State::kClosing;
  RetireAll();
  return true;
}

void Session::Closed() {
  RetireAll();
  state_ = State::kClosed;
}

void Session::Fail() {
  RetireAll();
  state_ = State::kFailed;
}

std::optional<Owner> Session::CreateView(uint64_t request) {
  if (state_ != State::kActive || !Next(next_view_) || !Next(next_request_))
    return std::nullopt;
  Owner owner{next_view_++, 0, request ? request : next_request_++, 1};
  owners_.emplace(owner.view, owner);
  return owner;
}

std::optional<Owner> Session::Navigate(uint64_t view, uint64_t request) {
  auto found = owners_.find(view);
  if (state_ != State::kActive || found == owners_.end() ||
      !Next(next_document_) || !Next(next_request_) ||
      !Next(found->second.generation))
    return std::nullopt;
  found->second.document = next_document_++;
  found->second.request = request ? request : next_request_++;
  ++found->second.generation;
  return found->second;
}

std::optional<Owner> Session::Find(uint64_t view) const {
  auto found = owners_.find(view);
  if (state_ != State::kActive || found == owners_.end())
    return std::nullopt;
  return found->second;
}

bool Session::Observe(const Owner& owner) {
  auto found = owners_.find(owner.view);
  if (state_ != State::kActive || found == owners_.end() || !owner.document ||
      !owner.request || owner.document <= found->second.document ||
      owner.generation <= found->second.generation)
    return false;
  found->second = owner;
  return true;
}

bool Session::ObserveGeneration(const Owner& owner) {
  auto found = owners_.find(owner.view);
  if (state_ != State::kActive || found == owners_.end() ||
      owner.document != found->second.document || owner.request != found->second.request ||
      owner.generation <= found->second.generation)
    return false;
  found->second = owner;
  return true;
}

bool Session::RetireView(uint64_t view) {
  return state_ == State::kActive && owners_.erase(view) != 0;
}

bool Session::Owns(const Owner& owner) const {
  auto found = owners_.find(owner.view);
  return state_ == State::kActive && found != owners_.end() &&
         found->second == owner;
}

std::optional<Owner> Session::RestartStream(uint64_t view) {
  auto found = owners_.find(view);
  if (state_ != State::kActive || found == owners_.end() ||
      !Next(found->second.generation))
    return std::nullopt;
  ++found->second.generation;
  return found->second;
}
}  // namespace alloy::guest
