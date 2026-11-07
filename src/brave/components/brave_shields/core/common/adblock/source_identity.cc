// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/components/brave_shields/core/common/adblock/source_identity.h"
#include <array>
#include "base/containers/span.h"
#include "crypto/hash.h"
namespace adblock {
rust::Vec<uint8_t> hash_filter_source(rust::Slice<const uint8_t> previous,
    rust::Slice<const uint8_t> rules, uint8_t permissions) {
  crypto::hash::Hasher hash(crypto::hash::kSha256);
  hash.Update("Alloy.AdBlock.Source.v1");
  hash.Update(base::span(previous.data(),previous.size()));
  std::array<uint8_t,9> frame{};frame[0]=permissions;
  uint64_t length=rules.size();
  for(size_t i=1;i<frame.size();++i){frame[i]=length&0xff;length>>=8;}
  hash.Update(frame);hash.Update(base::span(rules.data(),rules.size()));
  std::array<uint8_t,crypto::hash::kSha256Size> digest{};hash.Finish(digest);
  rust::Vec<uint8_t> result;for(auto byte:digest)result.push_back(byte);return result;
}
}
