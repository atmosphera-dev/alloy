// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/frame_pool.h"
#include "brave/alloy/guest/stream/resource_lane.h"
#include "brave/alloy/guest/session/extensions.h"
#include "brave/alloy/guest/session/session.h"
#include "brave/alloy/guest/stream/interaction.h"
#include "brave/alloy/guest/stream/popup.h"
#include "brave/alloy/guest/session/interactions.h"

#include <array>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::guest::stream {
namespace {
constexpr Owner kOwner{1, 2, 3, 4};
constexpr std::array<uint8_t, 16> kPixels = {1, 2, 3, 255, 4, 5, 6, 255,
                                           7, 8, 9, 255, 10, 11, 12, 255};
struct Pair {
  Pair() { EXPECT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds)); }
  ~Pair() { close(fds[0]); close(fds[1]); }
  int fds[2] = {-1, -1};
};
TEST(GuestFrames, BoundsAndOverflow) {
  size_t stride = 0, bytes = 0;
  EXPECT_FALSE(FrameBytes(0, 1, &stride, &bytes));
  EXPECT_FALSE(FrameBytes(-1, 1, &stride, &bytes));
  EXPECT_FALSE(FrameBytes(INT_MAX, INT_MAX, &stride, &bytes));
  ASSERT_TRUE(FrameBytes(1920, 1080, &stride, &bytes));
  EXPECT_EQ(7680u, stride);
  EXPECT_EQ(8294400u, bytes);
}
TEST(GuestFrames, CopyBeforeReturnAndReceiverWriteRefusal) {
  FramePool pool;
  ASSERT_TRUE(pool.Configure(1, 16));
  auto source = kPixels;
  auto frame = pool.Capture(kOwner, 1, 0, 2, 2, source.data(), 42);
  ASSERT_TRUE(frame);
  source.fill(0);
  std::array<uint8_t, 16> read = {};
  EXPECT_EQ(16, pread(pool.Handle(frame->token), read.data(), read.size(), 0));
  EXPECT_EQ(kPixels, read);
  EXPECT_EQ(-1, pwrite(pool.Handle(frame->token), read.data(), read.size(), 0));
  EXPECT_EQ(-1, ftruncate(pool.Handle(frame->token), 32));
  void* writer = mmap(nullptr, 16, PROT_WRITE, MAP_SHARED, pool.Handle(frame->token), 0);
  EXPECT_EQ(MAP_FAILED, writer);
  if (writer != MAP_FAILED) munmap(writer, 16);
}
TEST(GuestFrames, ExhaustionNeverOverwritesAndRecoveryIsFull) {
  FramePool pool;
  ASSERT_TRUE(pool.Configure(1, 16));
  auto first = pool.Capture(kOwner, 1, 0, 2, 2, kPixels.data(), 1);
  ASSERT_TRUE(first);
  auto second = kPixels;
  second.fill(99);
  EXPECT_FALSE(pool.Capture(kOwner, 1, 0, 2, 2, second.data(), 2));
  std::array<uint8_t, 16> read = {};
  EXPECT_EQ(16, pread(pool.Handle(first->token), read.data(), read.size(), 0));
  EXPECT_EQ(kPixels, read);
  ASSERT_TRUE(pool.Release(kOwner, first->token));
  auto recovered = pool.Capture(kOwner, 1, 0, 2, 2, second.data(), 3);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(3u, recovered->sequence);
  EXPECT_EQ(1u, recovered->dropped);
  EXPECT_NE(first->token, recovered->token);
  EXPECT_EQ(16, pread(pool.Handle(recovered->token), read.data(), read.size(), 0));
  EXPECT_EQ(second, read);
}
TEST(GuestFrames, StaleOrWrongOwnerCannotReleaseReusedSlot) {
  FramePool pool;
  ASSERT_TRUE(pool.Configure(1, 16));
  auto first = pool.Capture(kOwner, 1, 0, 2, 2, kPixels.data(), 1);
  ASSERT_TRUE(first);
  auto other = kOwner;
  ++other.document;
  EXPECT_FALSE(pool.Release(other, first->token));
  ASSERT_TRUE(pool.Release(kOwner, first->token));
  auto next = pool.Capture(other, 2, 0, 2, 2, kPixels.data(), 2);
  ASSERT_TRUE(next);
  EXPECT_FALSE(pool.Release(kOwner, first->token));
  EXPECT_FALSE(pool.Release(kOwner, next->token));
  EXPECT_FALSE(pool.Capture(other, 2, 0, 2, 2, kPixels.data(), 3));
  EXPECT_TRUE(pool.Release(other, next->token));
}
TEST(GuestFrames, ResizeBudgetIncludesOldLeasesAndCloseKeepsMappingAlive) {
  FramePool pool;
  ASSERT_TRUE(pool.Configure(2, 16));
  auto first = pool.Capture(kOwner, 1, 0, 2, 2, kPixels.data(), 1);
  ASSERT_TRUE(first);
  void* read = mmap(nullptr, 16, PROT_READ, MAP_SHARED, pool.Handle(first->token), 0);
  ASSERT_NE(MAP_FAILED, read);
  EXPECT_FALSE(pool.Capture(kOwner, 2, 0, 1, 1, kPixels.data(), 2));
  ASSERT_TRUE(pool.Release(kOwner, first->token));
  auto resized = pool.Capture(kOwner, 2, 0, 1, 1, kPixels.data(), 3);
  ASSERT_TRUE(resized);
  EXPECT_EQ(4u, resized->bytes);
  pool.Retire();
  EXPECT_FALSE(pool.Capture(kOwner, 3, 0, 2, 2, kPixels.data(), 4));
  EXPECT_EQ(kPixels[0], static_cast<const uint8_t*>(read)[0]);
  munmap(read, 16);
}
TEST(GuestFrames, SeparatePopupSequenceAndDroppedSend) {
  FramePool pool;
  ASSERT_TRUE(pool.Configure(2, 32));
  auto view = pool.Capture(kOwner, 1, 0, 2, 2, kPixels.data(), 1);
  auto popup = pool.Capture(kOwner, 1, 1, 2, 2, kPixels.data(), 2);
  ASSERT_TRUE(view); ASSERT_TRUE(popup);
  EXPECT_EQ(1u, view->sequence); EXPECT_EQ(1u, popup->sequence);
  pool.Drop(kOwner, popup->token);
  auto next = pool.Capture(kOwner, 1, 1, 2, 2, kPixels.data(), 3);
  ASSERT_TRUE(next); EXPECT_EQ(1u, next->dropped); EXPECT_EQ(2u, next->sequence);
}
TEST(GuestFrames, MetadataRoundtripRejectsWrongFormatStrideAndBounds) {
  FramePool pool;
  ASSERT_TRUE(pool.Configure(1, 16));
  auto frame = pool.Capture(kOwner, 1, 0, 2, 2, kPixels.data(), 42);
  ASSERT_TRUE(frame);
  std::string payload;
  ASSERT_TRUE(PackFrame(*frame, &payload));
  FrameInfo decoded;
  ASSERT_TRUE(UnpackFrame(kOwner, payload, &decoded));
  EXPECT_EQ(16u, decoded.bytes); EXPECT_EQ(42u, decoded.time_us);
  payload.back() = 'x';
  EXPECT_FALSE(UnpackFrame(kOwner, payload, &decoded));
  PackFields({"1", "1", "1", "0", "0", "0", "2", "2", "4", "16",
               "BGRA8-premultiplied-sRGB-full"}, &payload);
  EXPECT_FALSE(UnpackFrame(kOwner, payload, &decoded));
}
TEST(GuestResources, IdentitySequenceAndTransferredOwnership) {
  Pair sockets;
  SessionId id{1};
  ResourceLane producer(sockets.fds[0], id), consumer(sockets.fds[1], id);
  base::ScopedFD handle(open("/dev/null", O_RDONLY | O_CLOEXEC));
  ASSERT_TRUE(handle.is_valid());
  ASSERT_TRUE(producer.Send({Message::kExtensionResource, {}, 0, kOwner, "resource"}, {handle.get()}));
  Resource received;
  ASSERT_TRUE(consumer.Receive(&received));
  ASSERT_EQ(1u, received.handles.size());
  EXPECT_NE(-1, fcntl(received.handles[0].get(), F_GETFD));
  EXPECT_TRUE(fcntl(received.handles[0].get(), F_GETFD) & FD_CLOEXEC);
  EXPECT_EQ(kOwner, received.envelope.owner);
  EXPECT_EQ("resource", received.envelope.payload);
  EXPECT_FALSE(consumer.Receive(&received));
  EXPECT_EQ(EAGAIN, errno);
}
TEST(GuestResources, WrongIdentityClosesTransferredHandleAndRejects) {
  Pair sockets;
  ResourceLane producer(sockets.fds[0], SessionId{1}), consumer(sockets.fds[1], SessionId{2});
  base::ScopedFD handle(open("/dev/null", O_RDONLY | O_CLOEXEC));
  ASSERT_TRUE(producer.Send({}, {handle.get()}));
  Resource received;
  EXPECT_FALSE(consumer.Receive(&received));
  EXPECT_TRUE(received.handles.empty());
  EXPECT_EQ(EPROTO, errno);
}
TEST(GuestResources, SlowLaneSendReturnsWithoutWaiting) {
  Pair sockets;
  ResourceLane producer(sockets.fds[0], SessionId{1});
  bool blocked = false;
  for (size_t i = 0; i < 100000; ++i) {
    if (!producer.Send({Message::kExtensionResource, {}, 0, kOwner, std::string(8000, 'x')}, {})) {
      blocked = errno == EAGAIN || errno == EWOULDBLOCK;
      break;
    }
  }
  EXPECT_TRUE(blocked);
}
TEST(GuestExtensions, OneWireFormatForAllFeatures) {
  for (size_t i = 0; i < kFeatureNames.size(); ++i) {
    ExtensionRequest original{static_cast<Feature>(i), 123, "operation", std::string("a\0b", 3)};
    std::string payload;
    ASSERT_TRUE(PackExtension(original, &payload));
    ExtensionRequest decoded;
    ASSERT_TRUE(UnpackExtension(payload, &decoded));
    EXPECT_EQ(original.feature, decoded.feature);
    EXPECT_EQ(original.id, decoded.id);
    EXPECT_EQ(original.payload, decoded.payload);
    ASSERT_TRUE(PackExtensionReply({original.feature, original.id, ProviderResult::kOk,
                                    1234567, original.payload}, &payload));
    ExtensionResponse response;
    ASSERT_TRUE(UnpackExtensionReply(payload, &response));
    EXPECT_EQ(original.feature, response.feature);
    EXPECT_EQ(original.id, response.id);
    EXPECT_EQ(1234567u, response.completed_us);
    EXPECT_EQ(original.payload, response.payload);
  }
  ExtensionRequest decoded;
  std::string invalid;
  PackFields({"7", "1", "operation", "payload"}, &invalid);
  EXPECT_FALSE(UnpackExtension(invalid, &decoded));
}
TEST(GuestExtensions, ConcurrentCallsShareOwnerAndCompleteOnce) {
  Owner current = kOwner;
  std::vector<ProviderReply> delayed;
  Providers providers([&](const Owner& owner) { return owner == current; },
                      [](std::function<void()> task) { task(); });
  ASSERT_TRUE(providers.Install(Feature::kStream, StreamProvider(
      [&](const Owner&, const std::string&, const std::string&, ProviderReply reply) {
    delayed.push_back(std::move(reply));
  })));
  providers.Admit();
  int replies = 0;
  for (int i = 0; i < 2; ++i)
    providers.Invoke(Feature::kStream, current, "fixture", {},
        [&](ProviderResult result, std::string) {
      EXPECT_EQ(ProviderResult::kOk, result); ++replies;
    });
  ASSERT_EQ(2u, delayed.size());
  for (auto& reply : delayed) { reply(ProviderResult::kOk, {}); reply(ProviderResult::kOk, {}); }
  EXPECT_EQ(2, replies);
}
TEST(GuestExtensions, ResourceReleaseOutlivesDocumentButCannotDispatchContent) {
  Owner current = kOwner;
  int releases = 0, calls = 0;
  Providers providers([&](const Owner& owner) { return owner == current; },
                      [](std::function<void()> task) { task(); });
  Provider provider = StreamProvider([&](const Owner&, const std::string&,
                                         const std::string&, ProviderReply reply) {
    ++calls; reply(ProviderResult::kOk, {});
  });
  provider.release = [&](const Owner& original, const std::string& operation,
                          const std::string&, ProviderReply reply) {
    EXPECT_EQ(kOwner, original); EXPECT_EQ("@release", operation);
    ++releases; reply(ProviderResult::kOk, {});
  };
  ASSERT_TRUE(providers.Install(Feature::kStream, std::move(provider)));
  providers.Admit();
  ++current.document;
  providers.Invoke(Feature::kStream, kOwner, "input", {}, [](ProviderResult result, std::string) {
    EXPECT_EQ(ProviderResult::kCancelled, result);
  });
  providers.Invoke(Feature::kStream, kOwner, "@release", {}, [](ProviderResult result, std::string) {
    EXPECT_EQ(ProviderResult::kOk, result);
  });
  EXPECT_EQ(0, calls); EXPECT_EQ(1, releases);
  providers.Close();
  providers.Invoke(Feature::kStream, kOwner, "@release", {}, [](ProviderResult result, std::string) {
    EXPECT_EQ(ProviderResult::kCancelled, result);
  });
  EXPECT_EQ(1, releases);
}
TEST(GuestInteractions, ExactTimesSourcesAndOriginalTargetsRoundtrip) {
  InteractionData input{123, 456, 789, 1, 987654321, 123456, "agent", "pointer", "accepted", "{\"x\":12}"};
  std::string payload;
  ASSERT_TRUE(FitsInteraction(input));
  ASSERT_TRUE(PackInteraction(input, &payload));
  InteractionData result;
  ASSERT_TRUE(UnpackInteraction(payload, &result));
  EXPECT_EQ(input.id, result.id); EXPECT_EQ(input.document, result.document);
  EXPECT_EQ(input.generation, result.generation); EXPECT_EQ(input.surface, result.surface);
  EXPECT_EQ(input.capture_us, result.capture_us); EXPECT_EQ(input.dispatch_us, result.dispatch_us);
  EXPECT_EQ(input.details, result.details); EXPECT_EQ(input.source, result.source);
  input.generation = 0; ASSERT_TRUE(PackInteraction(input, &payload));
  EXPECT_FALSE(UnpackInteraction(payload, &result));
}
TEST(GuestInteractions, ReceiptSpaceIsReservedBeforeDispatchAdmission) {
  InteractionData input{1, 2, 3, 0, 4, 0, "app", "ime-set", {}, std::string(kMaxControlPayload, 'x')};
  EXPECT_FALSE(FitsInteraction(input));
  input.details = std::string(kMaxControlPayload - 256, 'x');
  EXPECT_TRUE(FitsInteraction(input));
}
TEST(GuestInteractions, EveryFeatureSharesIdsAndOneExactlyOnceReceipt) {
  Interactions tracker;
  tracker.BeginView(kOwner.view);
  for (size_t index = 0; index < kFeatureNames.size(); ++index) {
    auto id = tracker.Next(kOwner.view); ASSERT_TRUE(id); EXPECT_EQ(index + 1, *id);
    InteractionData input{*id, kOwner.document, 5, 0, 7, 0, "app", "fixture", {}, "{}"};
    auto feature = static_cast<Feature>(index);
    ASSERT_TRUE(tracker.Track(feature, kOwner, input));
    auto wrong = input; wrong.capture_us++; wrong.outcome = "accepted";
    EXPECT_EQ(Interactions::Receipt::kInvalid, tracker.Receive(feature, kOwner, wrong));
    input.outcome = "accepted"; input.dispatch_us = 123;
    EXPECT_EQ(Interactions::Receipt::kReady, tracker.Receive(feature, kOwner, input));
    EXPECT_EQ(Interactions::Receipt::kMissing, tracker.Receive(feature, kOwner, input));
    EXPECT_FALSE(tracker.Complete(kOwner.view, input.id));
  }
}
TEST(GuestInteractions, OldOwnersAndRetiredActionsCannotStealOrReviveReceipts) {
  Interactions tracker;
  tracker.BeginView(kOwner.view);
  InteractionData input{1, kOwner.document, 5, 0, 7, 0, "agent", "fixture", {}, "{}"};
  ASSERT_TRUE(tracker.Track(Feature::kStream, kOwner, input));
  auto receipt = input; receipt.outcome = "accepted";
  auto wrong = kOwner; wrong.generation++;
  EXPECT_EQ(Interactions::Receipt::kInvalid, tracker.Receive(Feature::kStream, wrong, receipt));
  auto completed = tracker.Complete(kOwner.view, 1); ASSERT_TRUE(completed);
  EXPECT_EQ(input.source, completed->input.source);
  EXPECT_EQ(Interactions::Receipt::kMissing, tracker.Receive(Feature::kStream, kOwner, receipt));
  input.id = 2; ASSERT_TRUE(tracker.Track(Feature::kAccessibility, kOwner, input));
  tracker.RetireView(kOwner.view);
  EXPECT_FALSE(tracker.Next(kOwner.view));
  EXPECT_FALSE(tracker.Complete(kOwner.view, 2));
  EXPECT_FALSE(tracker.Track(Feature::kStream, kOwner, input));
}
TEST(GuestWindows, NativeAndAppViewNamespacesCannotRace) {
  Session host, client;
  ProtectionReport report;
  report.bootstrap_before_engine = report.inherited_handles_closed = report.runtime_supervision = true;
  for (auto* session : {&host, &client}) {
    ASSERT_TRUE(session->Prepared(report)); ASSERT_TRUE(session->Accept(Protection::kUnavailable, true));
    ASSERT_TRUE(session->Activate());
  }
  auto child = host.CreateNativeView(); ASSERT_TRUE(child);
  EXPECT_GE(child->view, uint64_t{1} << 63);
  ASSERT_TRUE(client.ObserveNativeView(*child));
  EXPECT_FALSE(client.ObserveNativeView(*child));
  auto host_view = host.CreateView(1), client_view = client.CreateView(1);
  ASSERT_TRUE(host_view); ASSERT_TRUE(client_view); EXPECT_EQ(*host_view, *client_view);
  EXPECT_EQ(1u, host_view->view);
  auto unoffered = *host_view; EXPECT_FALSE(client.ObserveNativeView(unoffered));
}
TEST(GuestWindows, PendingChildIsAttachedOnceWithoutRecreation) {
  int offered = 0, attached = 0, denied = 0;
  Owner child{uint64_t{1} << 63, 0, uint64_t{1} << 63, 1};
  Popups popups([&](Owner opener, Owner value, std::string metadata) {
    EXPECT_EQ(kOwner, opener); EXPECT_EQ(child, value); EXPECT_EQ("original-window", metadata); ++offered;
  });
  popups.Add(kOwner, child, "original-window", [&](int width, int height, double scale) {
    EXPECT_EQ(192, width); EXPECT_EQ(100, height); EXPECT_EQ(1.5, scale); ++attached; return true;
  }, [&] { ++denied; });
  popups.Ready(child.view); popups.Ready(child.view); EXPECT_EQ(1, offered);
  std::string payload;
  PackFields({std::to_string(child.view), "1", "192", "100", "1.5"}, &payload);
  ASSERT_TRUE(popups.Call(kOwner, "window-decision", payload, [](ProviderResult result, std::string) {
    EXPECT_EQ(ProviderResult::kOk, result);
  }));
  EXPECT_EQ(1, attached); EXPECT_EQ(0, denied);
  popups.Call(kOwner, "window-decision", payload, [](ProviderResult result, std::string) {
    EXPECT_EQ(ProviderResult::kCancelled, result);
  });
  EXPECT_EQ(1, attached);
}
TEST(GuestWindows, StaleOpenerCannotAttachAndCancellationIsReentrant) {
  int attached = 0, denied = 0;
  Owner child{uint64_t{1} << 63, 0, uint64_t{1} << 63, 1};
  Popups popups([](Owner, Owner, std::string) {});
  popups.Add(kOwner, child, {}, [&](int, int, double) { ++attached; return true; }, [&] {
    ++denied; popups.Close();
  });
  popups.Ready(child.view);
  auto wrong = kOwner; ++wrong.document;
  std::string payload;
  PackFields({std::to_string(child.view), "1", "192", "100", "1.5"}, &payload);
  popups.Call(wrong, "window-decision", payload, [](ProviderResult result, std::string) {
    EXPECT_EQ(ProviderResult::kCancelled, result);
  });
  popups.Cancel(kOwner); popups.Cancel(kOwner);
  EXPECT_EQ(0, attached); EXPECT_EQ(1, denied);
}
}  // namespace
}  // namespace alloy::guest::stream
