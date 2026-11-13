// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_PROVIDERS_H_
#define BRAVE_ALLOY_GUEST_PROVIDERS_H_
#include <functional>
#include <array>
#include <string_view>
#include <map>
#include <memory>
#include <string>
#include "brave/alloy/guest/protocol/session.h"

namespace alloy::guest {
enum class Feature { kLinuxLock, kStream, kMedia, kIntegration,
                     kAccessibility, kNamespaceLock, kCapture };
inline constexpr std::array<std::string_view, 7> kFeatureNames = {
    "linux_lock", "stream", "media", "integration", "accessibility",
    "namespace_lock", "capture"};
inline constexpr uint64_t kKnownFeatures = (uint64_t{1} << kFeatureNames.size()) - 1;
enum class ProviderResult { kOk, kUnavailable, kCancelled, kFailed };
using ProviderReply = std::function<void(ProviderResult, std::string)>;
using ProviderCall = std::function<void(const Owner&, const std::string&,
                                       const std::string&, ProviderReply)>;

// No default native implementation. Each leaf owner injects its own operations
// without importing the session consumer. The control owner retains identity,
// cancellation and exactly-once completion independently of resource transport.
struct Provider {
  ProviderCall call;
  // Reserved @release only retires resources owned by the exact original
  // owner/token. It cannot admit content work and does not require a still-live
  // document: leases may outlive navigation/resize. Every leaf validates tokens.
  ProviderCall release = {};
  bool available() const { return !!call; }
};
Provider LinuxLockProvider(ProviderCall call = {});
Provider StreamProvider(ProviderCall call = {});
Provider MediaProvider(ProviderCall call = {});
Provider IntegrationProvider(ProviderCall call = {});
Provider AccessibilityProvider(ProviderCall call = {});
Provider NamespaceLockProvider(ProviderCall call = {});
Provider CaptureProvider(ProviderCall call = {});

class Providers {
 public:
  using Owns = std::function<bool(const Owner&)>;
  // Post must schedule onto the owning session sequence from any thread.
  using Post = std::function<void(std::function<void()>)>;
  Providers(Owns owns, Post post);
  ~Providers();
  Providers(const Providers&) = delete;
  Providers& operator=(const Providers&) = delete;
  // Install before admission, on the session sequence. A provider is never
  // installed automatically as a response to a failed/missing implementation.
  bool Install(Feature feature, Provider provider);
  void Admit();
  bool Available(Feature feature) const;
  void Invoke(Feature feature, Owner owner, const std::string& operation,
               const std::string& payload, ProviderReply reply);
  void Cancel(const Owner& owner);
  void Close();
 private:
  struct Pending;
  struct Lifetime;
  Owns owns_;
  Post post_;
  bool admitted_ = false;
  bool closed_ = false;
  std::map<Feature, Provider> providers_;
  std::map<uint64_t, std::shared_ptr<Pending>> pending_;
  uint64_t next_pending_ = 1;
  std::shared_ptr<Lifetime> lifetime_;
};
}  // namespace alloy::guest
#endif
