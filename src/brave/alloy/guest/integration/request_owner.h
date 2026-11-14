// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_INTEGRATION_REQUEST_OWNER_H_
#define BRAVE_ALLOY_GUEST_INTEGRATION_REQUEST_OWNER_H_

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "brave/alloy/guest/protocol/session.h"

namespace alloy::guest::integration {

// One native request registry. Admission is supplied by the native owner and
// rechecked on every operation, including data transfer, not just final reply.
// It stores pending requests only, never permissions or completed history.
class RequestOwner {
 public:
  using Admitted = std::function<bool()>;
  using Complete = std::function<void(bool, std::string,
                                     std::vector<std::string>)>;
  struct Request {
    Owner owner;
    Admitted admitted;
    Complete complete;
    std::vector<std::string> outgoing;
    std::vector<std::string> incoming;
    std::vector<uint64_t> expected;
    bool responding = false;
  };

  ~RequestOwner();
  uint64_t Begin(Owner owner, Admitted admitted, Complete complete,
                 std::vector<std::string> outgoing);
  std::optional<std::string> Read(Owner owner, uint64_t id, uint64_t index,
                                  uint64_t offset, uint64_t length);
  bool Respond(Owner owner, uint64_t id, std::vector<uint64_t> sizes);
  bool Append(Owner owner, uint64_t id, uint64_t index, uint64_t offset,
               std::string bytes);
  bool Finish(Owner owner, uint64_t id, bool allowed, std::string details);
  void Cancel(Owner owner);
  void Sweep();
  void Close();
  size_t pending() const { return requests_.size(); }

 private:
  Request* Find(Owner owner, uint64_t id);
  void Retire(uint64_t id);
  uint64_t next_ = 1;
  bool closed_ = false;
  std::map<uint64_t, Request> requests_;
};

}  // namespace alloy::guest::integration
#endif
