// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_EXTENSIONS_H_
#define BRAVE_ALLOY_GUEST_SESSION_EXTENSIONS_H_

#include <functional>
#include <memory>
#include <map>
#include <string>
#include "brave/alloy/guest/providers.h"
#include "brave/alloy/guest/stream/resource_lane.h"
#include "brave/alloy/guest/stream/interaction.h"

namespace alloy::guest {
// One internal route for all extension owners. Public typed adapters receive
// callbacks; they do not know or reach into the session implementation.
// Completion time is on the guest's session clock; zero means no native
// dispatch (for example a locally cancelled, unsent call). Typed producers
// report actual action dispatch separately, never equate completion with effect.
using ExtensionCompletion = std::function<void(ProviderResult, uint64_t, std::string)>;
using ExtensionCall = std::function<bool(Feature, std::string, std::string,
                                        ExtensionCompletion)>;
class ExtensionSink {
 public:
  virtual ~ExtensionSink() = default;
  virtual void Event(Owner owner, std::string operation, std::string payload) = 0;
  virtual void Resource(stream::Resource resource) = 0;
  virtual void OwnerChanged(Owner owner) = 0;
  virtual void ViewRetired(uint64_t view) = 0;
  virtual void Retire() = 0;
  virtual void Wait() = 0;
};
struct ExtensionEndpoint {
  ExtensionCall call;
  std::function<bool(Feature, std::shared_ptr<ExtensionSink>)> attach;
  std::function<bool(Feature, Owner, std::string)> release;
  std::function<bool(Feature, Owner, std::string, std::string,
                     ExtensionCompletion)> owned_call;
  // One per-view allocator/tracker for all action producers, not one private
  // counter/feed per stream, menu or AX adapter. Uses the same ordered call lane.
  std::function<uint64_t(Feature, Owner, std::string, stream::InteractionData)> interact;
};

struct ExtensionRequest {
  Feature feature = Feature::kStream;
  uint64_t id = 0;
  std::string operation;
  std::string payload;
};
bool PackExtension(const ExtensionRequest& request, std::string* payload);
bool UnpackExtension(const std::string& payload, ExtensionRequest* request);
struct ExtensionResponse {
  Feature feature = Feature::kStream;
  uint64_t id = 0;
  ProviderResult result = ProviderResult::kFailed;
  uint64_t completed_us = 0;
  std::string payload;
};
bool PackExtensionReply(const ExtensionResponse& response, std::string* payload);
bool UnpackExtensionReply(const std::string& payload, ExtensionResponse* response);
bool ParseUnsigned(const std::string& text, uint64_t* value);
bool ParseSigned(const std::string& text, int64_t* value);

// The reply registry owns completions until an authenticated matching reply
// takes them, or shutdown explicitly cancels them. Invalid replies cannot erase
// the callback that protocol-failure teardown still owes its caller.
class ExtensionReplies {
 public:
  struct Pending {
    Owner owner;
    Feature feature = Feature::kStream;
    ExtensionCompletion completion;
  };
  enum class Match { kUnknown, kInvalid, kReady };
  bool Add(uint64_t id, Pending pending);
  Match Take(uint64_t id, Owner owner, Feature feature, Pending* pending);
  void Cancel();
 private:
  std::map<uint64_t, Pending> pending_;
};
}  // namespace alloy::guest
#endif
