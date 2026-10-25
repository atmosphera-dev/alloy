// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "services/network/alloy_guard.h"

#include "base/alloy/storage_policy.h"
#include "base/containers/span.h"
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#endif

namespace network {
namespace {
constexpr AlloyGuard::Annotation kCatalog[] = {
#include "services/network/alloy_annotation_catalog.inc"
};
}  // namespace

AlloyGuard::AlloyGuard()
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    : log_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN}))
#endif
{}
AlloyGuard::~AlloyGuard() = default;

#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
void AlloyGuard::Configure(base::File log, Options options) {
  base::AutoLock lock(lock_);
  if (base::alloy::IsNoDiskProcess()) {
    log_.reset();
  } else {
    log_ = std::make_shared<base::File>(std::move(log));
  }
  options_ = options;
}
#endif

void AlloyGuard::SetOptions(Options options) {
  base::AutoLock lock(lock_);
  options_ = options;
}

base::span<const AlloyGuard::Annotation> AlloyGuard::Catalog() {
  return kCatalog;
}

AlloyGuard::Decision AlloyGuard::Classify(int hash) {
  std::string_view id = "unknown";
  for (const auto& row : kCatalog) {
    if (row.hash == hash) {
      id = row.id;
      break;
    }
  }
  return {false, id, net::ClassifyAlloyAnnotation(hash)};
}

std::string_view AlloyGuard::ClassName(TrafficClass category) {
  return net::AlloyTrafficClassName(category);
}

AlloyGuard::Decision AlloyGuard::DecideRequest(int hash,
                                            std::string_view endpoint,
                                            std::string_view source,
                                            int owner_hash) {
  Operation operation = Operation::kRequest;
  if (net::IsAlloyWorkerScriptLoad(hash)) {
    operation = Operation::kWorkerScript;
  } else if (net::IsAlloyDownloadContinuation(hash)) {
    operation = Operation::kDownloadContinuation;
  } else if (hash == Hash("dns_over_https") && owner_hash > 0) {
    operation = Operation::kOwnedDns;
  }
  return Decide(hash, endpoint, source, operation,
                owner_hash > 0 ? owner_hash : 0);
}

AlloyGuard::Decision AlloyGuard::Decide(int hash, std::string_view endpoint,
                                      std::string_view source,
                                      Operation operation, int owner_hash) {
  Decision result = Classify(hash);
  // Native DNS retains both labels: the transport never becomes a globally
  // allowed service, while permission and parental class come from its owner.
  if (operation == Operation::kOwnedDns) {
    const auto origin = Classify(owner_hash);
    result.originating_annotation_id = origin.annotation_id;
    result.traffic_class = origin.traffic_class;
  } else if (operation == Operation::kWorkerScript) {
    result.originating_annotation_id = Classify(owner_hash).annotation_id;
    if (net::IsAlloyPageWorkerOwner(owner_hash))
      result.traffic_class = TrafficClass::kPageLoad;
  } else if (operation == Operation::kDownloadContinuation) {
    const auto origin = Classify(owner_hash);
    result.originating_annotation_id = origin.annotation_id;
    result.traffic_class = net::IsAlloyPageDownloadOwner(owner_hash)
        ? TrafficClass::kPageDownload : origin.traffic_class;
  }
  base::AutoLock lock(lock_);
  switch (result.traffic_class) {
    case TrafficClass::kPageLoad:
    case TrafficClass::kPageDownload:
    case TrafficClass::kServiceWorker:
    case TrafficClass::kWebSocket:
    case TrafficClass::kWebTransport:
    case TrafficClass::kWebRtc:
      result.allowed = true;
      break;
    case TrafficClass::kOnBehalfOfPage: {
      const auto policy_id = operation == Operation::kOwnedDns
          ? result.originating_annotation_id : result.annotation_id;
      result.allowed =
          (policy_id == "favicon_loader" && options_.favicons) ||
          (policy_id == "open_search" && options_.open_search) ||
          (policy_id == "alloy_dns_prefetch" && options_.dns_prefetch) ||
           ((policy_id == "predictive_preconnect" ||
             policy_id == "search_engine_preconnect" ||
             policy_id == "network_hints_preconnect") && options_.preconnect);
      break;
    }
    case TrafficClass::kBrowserService:
    case TrafficClass::kUnannotated:
      break;
  }
  if (operation == Operation::kStandaloneDns)
    result.allowed = result.allowed && options_.dns_prefetch;
  if (operation == Operation::kPreconnect)
    result.allowed = result.allowed && options_.preconnect;
  // A capability cannot be relabelled by a child to escape its owner's class.
  if (operation == Operation::kOwnedDns) {
    result.allowed = result.allowed && owner_hash > 0 &&
        (hash == Hash("alloy_native_dns") || hash == Hash("dns_over_https"));
  } else if (operation == Operation::kWorkerScript) {
    result.allowed = result.allowed && net::IsAlloyWorkerScriptLoad(hash) &&
        net::IsAlloyPageWorkerOwner(owner_hash);
  } else if (operation == Operation::kDownloadContinuation) {
    result.allowed = result.allowed &&
        net::IsAlloyDownloadContinuation(hash) &&
        net::IsAlloyPageDownloadOwner(owner_hash);
  } else if (owner_hash && (Classify(owner_hash).traffic_class != result.traffic_class ||
                     (owner_hash != hash &&
                      result.traffic_class != TrafficClass::kWebRtc)))
    result.allowed = false;
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  if (!result.allowed && !base::alloy::IsNoDiskProcess()) {
    base::DictValue row;
    row.Set("time", base::Time::Now().InMillisecondsFSinceUnixEpoch());
    row.Set("annotation_id", result.annotation_id);
    row.Set("annotation_hash", hash);
    if (operation == Operation::kWorkerScript || operation == Operation::kOwnedDns ||
        operation == Operation::kDownloadContinuation) {
      row.Set("originating_annotation_id", result.originating_annotation_id);
      row.Set("originating_annotation_hash", owner_hash);
    }
    row.Set("class", ClassName(result.traffic_class));
    row.Set("endpoint", endpoint);
    row.Set("entry_point", source);
    std::string json;
    CHECK(base::JSONWriter::Write(row, &json));
    json += '\n';
    if (log_ && log_->IsValid()) {
      log_runner_->PostTask(FROM_HERE, base::BindOnce(
          [](std::shared_ptr<base::File> file, std::string line) {
            CHECK(file->WriteAtCurrentPosAndCheck(base::as_byte_span(line)))
                << "Cannot persist Alloy guard denial";
          }, log_, std::move(json)));
    } else {
      LOG(ERROR) << "Alloy guard has no log descriptor: " << json;
    }
  }
#endif
  return result;
}
}  // namespace network
