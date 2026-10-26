// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef SERVICES_NETWORK_ALLOY_GUARD_H_
#define SERVICES_NETWORK_ALLOY_GUARD_H_

#include <cstdint>
#include <string_view>

#include "base/containers/span.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "net/base/alloy_traffic_class.h"
#include "services/network/alloy_buildflags.h"

#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
#include <memory>
#include "base/files/file.h"
#include "base/task/sequenced_task_runner.h"
#endif

namespace network {

// One policy owner per NetworkContext; children retain it, never a cached allow.
// Classification is independent of permission so later parental policy can
// deny a class (for example WebRTC) without reconstructing call-site semantics.
class AlloyGuard : public base::RefCountedThreadSafe<AlloyGuard> {
 public:
  using TrafficClass = net::AlloyTrafficClass;
  enum class Operation {
    kRequest, kStandaloneDns, kPreconnect, kOwnedDns, kDownloadContinuation,
    kWorkerScript
  };
  struct Options {
    bool favicons = true;
    bool open_search = false;
    bool dns_prefetch = false;
    bool preconnect = false;
  };
  struct Decision {
    bool allowed;
    std::string_view annotation_id;
    TrafficClass traffic_class;
    std::string_view originating_annotation_id;
  };
  struct Annotation { int hash; std::string_view id; };

  enum class RecordingMode { kPermitted, kForbidden };
  explicit AlloyGuard(RecordingMode recording_mode = RecordingMode::kPermitted);
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  // The browser opens an append-only log before passing the FD into the
  // sandbox. A network child never opens a filesystem path or gains directory
  // access just to log a denial.
  void Configure(base::File log, Options options);
#endif
  void SetOptions(Options options);
  Decision Decide(int annotation_hash, std::string_view endpoint,
                  std::string_view source,
                  Operation operation = Operation::kRequest,
                  int owner_annotation_hash = 0);
  // Shared request-boundary dispatch: factories, URLRequest and transactions
  // must not independently interpret transport/continuation ownership.
  Decision DecideRequest(int annotation_hash, std::string_view endpoint,
                         std::string_view source,
                         int owner_annotation_hash = 0);

  static constexpr int Hash(std::string_view id) {
    return net::AlloyAnnotationHash(id);
  }
  static Decision Classify(int hash);
  static std::string_view ClassName(TrafficClass traffic_class);
  static base::span<const Annotation> Catalog();

 private:
  friend class base::RefCountedThreadSafe<AlloyGuard>;
  ~AlloyGuard();
  base::Lock lock_;
  Options options_ GUARDED_BY(lock_);
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  const bool recording_permitted_;
  std::shared_ptr<base::File> log_ GUARDED_BY(lock_);
  const scoped_refptr<base::SequencedTaskRunner> log_runner_;
#endif
};

}  // namespace network
#endif  // SERVICES_NETWORK_ALLOY_GUARD_H_
