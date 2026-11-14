// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_INTERACTION_H_
#define BRAVE_ALLOY_GUEST_STREAM_INTERACTION_H_
#include "brave/alloy/guest/protocol/session.h"
namespace alloy::guest::stream {
inline constexpr char kInteractionOperation[] = "@interaction";
struct InteractionData {
  uint64_t id = 0, document = 0, generation = 0, surface = 0;
  uint64_t capture_us = 0, dispatch_us = 0;
  std::string source, kind, outcome, details;
};
bool PackInteraction(const InteractionData& input, std::string* payload);
bool UnpackInteraction(const std::string& payload, InteractionData* input);
// Admit only requests whose largest dispatch receipt also fits the existing
// logical control envelope. Never perform an action that cannot be receipted.
bool FitsInteraction(const InteractionData& input);
}  // namespace alloy::guest::stream
#endif
