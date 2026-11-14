// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/request_owner.h"

#include <utility>

namespace alloy::guest::integration {

RequestOwner::~RequestOwner() { Close(); }

uint64_t RequestOwner::Begin(Owner owner, Admitted admitted, Complete complete,
                              std::vector<std::string> outgoing) {
  if (closed_ || !next_ || !owner.document || !admitted || !complete ||
      !admitted()) {
    if (complete) complete(false, {}, {});
    return 0;
  }
  const uint64_t id = next_++;
  requests_.emplace(id, Request{owner, std::move(admitted), std::move(complete),
                                std::move(outgoing), {}, {}, false});
  return id;
}

RequestOwner::Request* RequestOwner::Find(Owner owner, uint64_t id) {
  auto found = requests_.find(id);
  if (found == requests_.end() || found->second.owner != owner) return nullptr;
  if (!found->second.admitted()) {
    Retire(id);
    return nullptr;
  }
  return &found->second;
}

std::optional<std::string> RequestOwner::Read(
    Owner owner, uint64_t id, uint64_t index, uint64_t offset, uint64_t length) {
  auto* request = Find(owner, id);
  if (!request || index >= request->outgoing.size()) return std::nullopt;
  const auto& bytes = request->outgoing[index];
  if (offset > bytes.size() || length > bytes.size() - offset)
    return std::nullopt;
  return bytes.substr(static_cast<size_t>(offset), static_cast<size_t>(length));
}

bool RequestOwner::Respond(Owner owner, uint64_t id,
                            std::vector<uint64_t> sizes) {
  auto* request = Find(owner, id);
  if (!request || request->responding) return false;
  // Do not reserve untrusted declared extents. Only received, validated bytes
  // allocate storage; all arithmetic is checked before appending.
  for (auto size : sizes)
    if (size > std::string().max_size()) return false;
  request->responding = true;
  request->expected = std::move(sizes);
  request->incoming.resize(request->expected.size());
  return true;
}

bool RequestOwner::Append(Owner owner, uint64_t id, uint64_t index,
                           uint64_t offset, std::string bytes) {
  auto* request = Find(owner, id);
  if (!request || !request->responding || index >= request->incoming.size())
    return false;
  auto& target = request->incoming[index];
  const auto expected = request->expected[index];
  if (offset != target.size() || offset > expected ||
      bytes.size() > expected - offset)
    return false;
  target.append(bytes);
  return true;
}

bool RequestOwner::Finish(Owner owner, uint64_t id, bool allowed,
                           std::string details) {
  auto* request = Find(owner, id);
  if (!request) return false;
  if (allowed) {
    if (!request->responding) return false;
    for (size_t index = 0; index < request->incoming.size(); ++index)
      if (request->incoming[index].size() != request->expected[index]) return false;
  }
  auto complete = std::move(request->complete);
  auto incoming = allowed ? std::move(request->incoming)
                          : std::vector<std::string>();
  // Remove authority before calling native code: callbacks may reenter, retire
  // the view or start another request, but cannot complete this one twice.
  requests_.erase(id);
  complete(allowed, allowed ? std::move(details) : std::string(),
           std::move(incoming));
  return true;
}

void RequestOwner::Retire(uint64_t id) {
  auto found = requests_.find(id);
  if (found == requests_.end()) return;
  auto complete = std::move(found->second.complete);
  requests_.erase(found);
  complete(false, {}, {});
}

void RequestOwner::Cancel(Owner owner) {
  std::vector<Complete> completions;
  for (auto found = requests_.begin(); found != requests_.end();) {
    if (found->second.owner == owner) {
      completions.push_back(std::move(found->second.complete));
      found = requests_.erase(found);
    } else {
      ++found;
    }
  }
  // No member access after native callbacks start: one may destroy this owner.
  for (auto& complete : completions) complete(false, {}, {});
}

void RequestOwner::Sweep() {
  std::vector<Complete> completions;
  for (auto found = requests_.begin(); found != requests_.end();) {
    if (!found->second.admitted()) {
      completions.push_back(std::move(found->second.complete));
      found = requests_.erase(found);
    } else {
      ++found;
    }
  }
  for (auto& complete : completions) complete(false, {}, {});
}

void RequestOwner::Close() {
  closed_ = true;
  auto requests = std::move(requests_);
  requests_.clear();
  for (auto& [id, request] : requests) request.complete(false, {}, {});
}

}  // namespace alloy::guest::integration
