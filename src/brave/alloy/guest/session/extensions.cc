// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/extensions.h"

#include <charconv>
#include <utility>

namespace alloy::guest {
bool ParseUnsigned(const std::string& text, uint64_t* value) {
  if (text.empty()) return false;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}
bool ParseSigned(const std::string& text, int64_t* value) {
  if (text.empty()) return false;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}
bool PackExtension(const ExtensionRequest& request, std::string* payload) {
  return PackFields({std::to_string(static_cast<int>(request.feature)),
                     std::to_string(request.id), request.operation,
                     request.payload}, payload);
}
bool UnpackExtension(const std::string& payload, ExtensionRequest* request) {
  std::vector<std::string> fields;
  uint64_t feature = 0, id = 0;
  if (!UnpackFields(payload, 4, &fields) ||
      !ParseUnsigned(fields[0], &feature) || feature >= kFeatureNames.size() ||
      !ParseUnsigned(fields[1], &id) || fields[2].empty())
    return false;
  *request = {static_cast<Feature>(feature), id, std::move(fields[2]),
              std::move(fields[3])};
  return true;
}
bool PackExtensionReply(const ExtensionResponse& response, std::string* payload) {
  return PackFields({std::to_string(static_cast<int>(response.feature)),
                     std::to_string(response.id),
                     std::to_string(static_cast<int>(response.result)),
                     std::to_string(response.completed_us), response.payload}, payload);
}
bool UnpackExtensionReply(const std::string& payload, ExtensionResponse* response) {
  std::vector<std::string> fields;
  uint64_t feature = 0, id = 0, result = 0, time = 0;
  if (!UnpackFields(payload, 5, &fields) ||
      !ParseUnsigned(fields[0], &feature) || feature >= kFeatureNames.size() ||
      !ParseUnsigned(fields[1], &id) || !id ||
      !ParseUnsigned(fields[2], &result) || result > static_cast<uint64_t>(ProviderResult::kFailed) ||
      !ParseUnsigned(fields[3], &time)) return false;
  *response = {static_cast<Feature>(feature), id, static_cast<ProviderResult>(result),
               time, std::move(fields[4])};
  return true;
}
}  // namespace alloy::guest
