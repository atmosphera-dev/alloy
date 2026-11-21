// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/namespace_lock/packaging_protection.h"
#include <utility>
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace alloy::guest {
std::optional<std::array<std::string, 5>> PackagingProtectionFields(
    int abi, const LinuxSandboxFacts& sandbox, bool flatpak_broker) {
  auto common = sandbox;
  if (flatpak_broker) {
    if (sandbox.route != "flatpak-brokered-userns") return std::nullopt;
    // Reuse the native backend's ABI/NNP/seccomp/capability requirements. The
    // emitted route below is the actual brokered route, never a native-unshare
    // claim. Only the sealed bootstrap packaging fact selects this backend.
    common.route = "userns";
  }
  auto fields = LinuxProtectionFields(abi, common);
  if (!fields) return std::nullopt;
  auto coverage = base::JSONReader::ReadDict((*fields)[1], base::JSON_PARSE_RFC);
  auto native = base::JSONReader::ReadDict((*fields)[3], base::JSON_PARSE_RFC);
  auto limits = base::JSONReader::ReadDict((*fields)[4], base::JSON_PARSE_RFC);
  if (!coverage || !native || !limits) return std::nullopt;
  auto* trusted = coverage->FindList("trusted_setup");
  if (!trusted) return std::nullopt;
  trusted->Append("fixed-static-bootstrap-teardown-probe; no-engine-payload");
  trusted->Append("fixed-static-wait-owner; private-kernel-exit-status; no-engine-payload");
  coverage->Set("snap_external_entries", "unsupported:strict-Snap-policy-denies-pidfd_send_signal");
  limits->Set("namespace_filesystem_and_packages",
      "native-namespace-view-absent; Snap-unsupported-policy-denies-pidfd_send_signal");
  if (flatpak_broker) {
    coverage->Set("flatpak_external_entries", "fixed-bootstrap-per-entry; explicit-verified-RAM-and-launch-capabilities");
    trusted->Append("Flatpak-portal,bubblewrap,pinned-unprivileged-zypak-setup; no-engine-payload");
    native->Set("route", sandbox.route);
    native->Set("setuid_helper", "never-used; zypak-adapts-the-native-helper-handshake-to-Flatpak-user-namespaces");
    limits->Set("namespace_filesystem_and_packages",
        "native-namespace-view-absent; Flatpak-brokered-entries; Snap-unsupported-policy-denies-pidfd_send_signal");
  }
  auto encoded_coverage = base::WriteJson(*coverage);
  auto encoded_native = base::WriteJson(*native);
  auto encoded_limits = base::WriteJson(*limits);
  if (!encoded_coverage || !encoded_native || !encoded_limits) return std::nullopt;
  (*fields)[1] = std::move(*encoded_coverage);
  (*fields)[3] = std::move(*encoded_native);
  (*fields)[4] = std::move(*encoded_limits);
  return fields;
}
}
