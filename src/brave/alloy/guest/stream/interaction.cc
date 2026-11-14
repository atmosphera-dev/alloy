// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/interaction.h"
#include "brave/alloy/guest/session/extensions.h"
#include <limits>
namespace alloy::guest::stream {
bool PackInteraction(const InteractionData& input, std::string* payload) {
  return PackFields({std::to_string(input.id), std::to_string(input.document),
      std::to_string(input.generation), std::to_string(input.surface),
      std::to_string(input.capture_us), std::to_string(input.dispatch_us),
      input.source, input.kind, input.outcome, input.details}, payload);
}
bool UnpackInteraction(const std::string& payload, InteractionData* input) {
  std::vector<std::string> fields;
  uint64_t numbers[6] = {};
  if (!UnpackFields(payload, 10, &fields)) return false;
  for (size_t i = 0; i < 6; ++i)
    if (!ParseUnsigned(fields[i], &numbers[i])) return false;
  if (!numbers[0] || !numbers[2] || numbers[3] > 1 || fields[6].empty() || fields[7].empty())
    return false;
  *input = {numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5],
            std::move(fields[6]), std::move(fields[7]), std::move(fields[8]), std::move(fields[9])};
  return true;
}
bool FitsInteraction(const InteractionData& input) {
  std::string payload, envelope;
  if (!PackInteraction(input, &payload) ||
      !PackExtension({Feature::kStream, std::numeric_limits<uint64_t>::max(), "input", payload}, &envelope)) return false;
  auto receipt = input;
  receipt.dispatch_us = std::numeric_limits<uint64_t>::max();
  receipt.outcome = "rejected:document-not-ready";
  return PackInteraction(receipt, &payload) &&
      PackExtension({Feature::kStream, 0, kInteractionOperation, payload}, &envelope);
}
}  // namespace alloy::guest::stream
