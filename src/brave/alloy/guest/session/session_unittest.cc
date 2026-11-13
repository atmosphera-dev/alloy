// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/session/session.h"
#include "brave/alloy/guest/providers.h"
#include "brave/alloy/guest/session/control_queue.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <deque>
#include <memory>
#include <cstring>
#include <utility>

#include "testing/gtest/include/gtest/gtest.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/task_environment.h"

namespace alloy::guest {
namespace {
SessionId Id(uint8_t byte = 1) {
  SessionId id = {};
  id[0] = byte;
  return id;
}
ProtectionReport Facts() {
  ProtectionReport report;
  report.bootstrap_before_engine = true;
  report.inherited_handles_closed = true;
  report.runtime_supervision = true;
  return report;
}
Session Active() {
  Session session;
  EXPECT_TRUE(session.Prepared(Facts()));
  EXPECT_TRUE(session.Accept(Protection::kUnavailable, true));
  EXPECT_TRUE(session.Activate());
  return session;
}
class Channel {
 public:
  Channel() { EXPECT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds)); }
  ~Channel() { close(fds[0]); close(fds[1]); }
  int fds[2] = {-1, -1};
};
size_t OpenDescriptors() {
  DIR* directory = opendir("/proc/self/fd");
  EXPECT_NE(nullptr, directory);
  size_t count = 0;
  while (auto* entry = readdir(directory))
    if (entry->d_name[0] != '.') ++count;
  closedir(directory);
  return count;
}

TEST(GuestProtocol, ExplicitBigEndianEnvelopeRoundTrips) {
  Envelope input{Message::kNavigate, Id(), 0x1020304050607080,
                  {11, 12, 13, 14}, std::string("a\0b", 3)};
  std::vector<uint8_t> wire;
  ASSERT_TRUE(Encode(input, &wire));
  EXPECT_EQ(kEnvelopeSize + 3, wire.size());
  EXPECT_EQ(0x10, wire[32]);
  EXPECT_EQ(0x80, wire[39]);
  Envelope output;
  ASSERT_TRUE(Decode(wire, &output));
  EXPECT_EQ(input.message, output.message);
  EXPECT_EQ(input.session, output.session);
  EXPECT_EQ(input.owner, output.owner);
  EXPECT_EQ(input.payload, output.payload);
}
TEST(GuestProtocol, VersionFlagsLengthUnknownKindAndZeroIdentityFailClosed) {
  Envelope input{Message::kHello, Id(), 1, {}, "fixture"};
  std::vector<uint8_t> wire;
  ASSERT_TRUE(Encode(input, &wire));
  Envelope output;
  for (size_t offset : {size_t{0}, size_t{5}, size_t{7}, size_t{8}, size_t{12}}) {
    auto bad = wire;
    bad[offset] = 255;
    EXPECT_FALSE(Decode(bad, &output)) << offset;
  }
  wire.resize(kEnvelopeSize - 1);
  EXPECT_FALSE(Decode(wire, &output));
  input.session = {};
  EXPECT_FALSE(Encode(input, &wire));
  input.session = Id();
  input.sequence = 0;
  EXPECT_FALSE(Encode(input, &wire));
}
TEST(GuestProtocol, FieldsRejectTrailingAndTruncatedInputWithoutPartialResult) {
  std::string payload;
  ASSERT_TRUE(PackFields({"one", std::string("\0two", 4)}, &payload));
  std::vector<std::string> fields;
  ASSERT_TRUE(UnpackFields(payload, 2, &fields));
  EXPECT_EQ(std::string("\0two", 4), fields[1]);
  EXPECT_FALSE(UnpackFields(payload, 1, &fields));
  EXPECT_FALSE(UnpackFields(payload.substr(0, payload.size() - 1), 2, &fields));
  EXPECT_EQ("one", fields[0]);
  EXPECT_FALSE(PackFields({std::string(kMaxControlPayload, 'x')}, &payload));
}
TEST(GuestProtocol, ControlFieldsAreNotLimitedToOneSocketPacket) {
  // Chromium's pinned URL limit is 2 MiB, not a 16-bit string/packet limit.
  std::string payload;
  ASSERT_TRUE(PackFields({std::string(192 * 1024, 'x')}, &payload));
  Envelope input{Message::kNavigate, Id(), 1, {1, 1, 1, 1}, payload};
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(Encode(input, &bytes));
  Envelope output;
  ASSERT_TRUE(Decode(bytes, &output));
  EXPECT_EQ(input.payload, output.payload);
}
TEST(GuestProtocol, NonblockingChunkedControlPreservesEnvelopeAndSequence) {
  Channel channel;
  ASSERT_EQ(0, fcntl(channel.fds[0], F_SETFL, O_NONBLOCK));
  ASSERT_EQ(0, fcntl(channel.fds[1], F_SETFL, O_NONBLOCK));
  Peer sender(Id()), receiver(Id());
  Envelope input{Message::kNavigate, {}, 0, {1, 2, 3, 4}, {}};
  ASSERT_TRUE(PackFields({std::string(2 * 1024 * 1024, 'x')}, &input.payload));
  bool sent = false, received = false;
  Envelope output;
  while (!sent || !received) {
    if (!sent) {
      sent = sender.Send(channel.fds[0], input);
      if (!sent) {
        ASSERT_EQ(EAGAIN, errno);
      }
    }
    if (!received) {
      received = receiver.Receive(channel.fds[1], &output);
      if (!received) {
        ASSERT_EQ(EAGAIN, errno);
      }
    }
  }
  EXPECT_EQ(1u, output.sequence);
  EXPECT_EQ(input.payload, output.payload);
  ASSERT_TRUE(sender.Send(channel.fds[0], {Message::kClose, {}, 0, {}, {}}));
  ASSERT_TRUE(receiver.Receive(channel.fds[1], &output));
  EXPECT_EQ(2u, output.sequence);
  EXPECT_EQ(Message::kClose, output.message);
}
TEST(GuestProtocol, PeerRejectsWrongSessionAndRepeatedSequence) {
  Channel channel;
  Peer peer(Id());
  Envelope output;
  EXPECT_TRUE(SendEnvelope(channel.fds[0], {Message::kHello, Id(2), 1, {}, {}}));
  EXPECT_FALSE(peer.Receive(channel.fds[1], &output));
  EXPECT_TRUE(SendEnvelope(channel.fds[0], {Message::kHello, Id(), 1, {}, {}}));
  EXPECT_TRUE(peer.Receive(channel.fds[1], &output));
  EXPECT_TRUE(SendEnvelope(channel.fds[0], {Message::kHello, Id(), 1, {}, {}}));
  EXPECT_FALSE(peer.Receive(channel.fds[1], &output));
}
TEST(GuestProtocol, UnexpectedTransferredHandlesAreClosedNotInterpretedAsIds) {
  Channel channel;
  int handle = open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(handle, 0);
  size_t before = OpenDescriptors();
  std::vector<uint8_t> wire;
  ASSERT_TRUE(Encode({Message::kHello, Id(), 1, {}, {}}, &wire));
  iovec io = {wire.data(), wire.size()};
  alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))] = {};
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = ancillary;
  message.msg_controllen = sizeof(ancillary);
  cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &handle, sizeof(handle));
  ASSERT_EQ(static_cast<ssize_t>(wire.size()), sendmsg(channel.fds[0], &message, 0));
  Envelope output;
  EXPECT_FALSE(ReceiveEnvelope(channel.fds[1], &output));
  EXPECT_EQ(before, OpenDescriptors());
  close(handle);
}
TEST(GuestSession, FactsAndExplicitAcceptancePrecedeAdmission) {
  Session session;
  EXPECT_FALSE(session.CreateView());
  EXPECT_FALSE(session.Activate());
  auto report = Facts();
  report.bootstrap_before_engine = false;
  EXPECT_FALSE(session.Prepared(report));
  EXPECT_TRUE(session.Prepared(Facts()));
  EXPECT_FALSE(session.CreateView());
  EXPECT_FALSE(session.Accept(Protection::kDataWritesDenied, true));
  EXPECT_FALSE(session.Accept(Protection::kUnavailable, false));
  EXPECT_TRUE(session.Accept(Protection::kUnavailable, true));
  EXPECT_FALSE(session.Accept(Protection::kUnavailable, true));
  EXPECT_TRUE(session.Activate());
  EXPECT_TRUE(session.CreateView());
}
TEST(GuestSession, IndependentSessionsShareNoOwners) {
  auto first = Active();
  auto second = Active();
  auto view = first.CreateView();
  ASSERT_TRUE(view);
  EXPECT_FALSE(second.Owns(*view));
  auto other_view = first.CreateView();
  ASSERT_TRUE(other_view);
  EXPECT_NE(view->view, other_view->view);
  EXPECT_TRUE(first.Owns(*view));
}
TEST(GuestSession, NavigationAndStreamRecoveryRetireOriginalGeneration) {
  auto session = Active();
  auto view = session.CreateView();
  ASSERT_TRUE(view);
  auto document = session.Navigate(view->view);
  ASSERT_TRUE(document);
  EXPECT_FALSE(session.Owns(*view));
  EXPECT_TRUE(session.Owns(*document));
  auto recovery = session.RestartStream(view->view);
  ASSERT_TRUE(recovery);
  EXPECT_EQ(document->document, recovery->document);
  EXPECT_GT(recovery->generation, document->generation);
  EXPECT_FALSE(session.Owns(*document));
  EXPECT_TRUE(session.Owns(*recovery));
  EXPECT_TRUE(session.RetireView(view->view));
  EXPECT_FALSE(session.Owns(*recovery));
  EXPECT_FALSE(session.Navigate(view->view));
}
TEST(GuestSession, ClientObservesNativeDocumentsNotEnqueuedUrls) {
  auto session = Active();
  auto view = session.CreateView();
  ASSERT_TRUE(view);
  Owner native{view->view, 101, 20, 2};
  EXPECT_TRUE(session.Observe(native));
  EXPECT_FALSE(session.Observe(native));
  EXPECT_FALSE(session.Observe({view->view, 100, 21, 3}));
  EXPECT_FALSE(session.Owns(*view));
  EXPECT_TRUE(session.Owns(native));
  Owner recovery = native;
  ++recovery.generation;
  EXPECT_TRUE(session.ObserveGeneration(recovery));
  EXPECT_FALSE(session.Owns(native));
  EXPECT_TRUE(session.Owns(recovery));
  EXPECT_FALSE(session.ObserveGeneration(native));
  ++recovery.request;
  ++recovery.generation;
  EXPECT_FALSE(session.ObserveGeneration(recovery));
}
TEST(GuestSession, CloseFenceRefusesQueuedAdmissionAndCannotResurrectOwners) {
  auto session = Active();
  auto view = session.CreateView();
  ASSERT_TRUE(view);
  EXPECT_TRUE(session.BeginClose());
  EXPECT_FALSE(session.BeginClose());
  EXPECT_FALSE(session.CreateView());
  EXPECT_FALSE(session.Navigate(view->view));
  EXPECT_FALSE(session.RestartStream(view->view));
  EXPECT_FALSE(session.Owns(*view));
  session.Closed();
  EXPECT_FALSE(session.Prepared(Facts()));
  EXPECT_FALSE(session.Accept(Protection::kUnavailable, true));
}
TEST(GuestSession, FailureRetiresEveryOutstandingOwner) {
  auto session = Active();
  auto first = session.CreateView();
  auto second = session.CreateView();
  session.Fail();
  EXPECT_EQ(State::kFailed, session.state());
  EXPECT_FALSE(session.Owns(*first));
  EXPECT_FALSE(session.Owns(*second));
  EXPECT_FALSE(session.Activate());
}
TEST(GuestSession, NavigationOwnershipIsIndependentOfGpuStreamRecovery) {
  Owner navigation{1, 2, 3, 4};
  Owner recovered = navigation;
  ++recovered.generation;
  EXPECT_TRUE(SameNavigation(navigation, recovered));
  ++recovered.document;
  EXPECT_FALSE(SameNavigation(navigation, recovered));
  recovered = navigation;
  ++recovered.request;
  EXPECT_FALSE(SameNavigation(navigation, recovered));
}
TEST(GuestControlQueue, SlowConsumerStopsFurtherProducerBatches) {
  base::test::SingleThreadTaskEnvironment tasks;
  ControlQueue queue;
  int batches = 0;
  for (int task = 0; task < 10000; ++task) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce([](ControlQueue* queue, int* batches) {
          ++*batches;
          queue->Push({Message::kActive, {}, 0, {}, {}});
        }, base::Unretained(&queue), base::Unretained(&batches)));
  }
  for (int attempt = 0; attempt < 10000; ++attempt) {
    queue.Pump();
  }
  EXPECT_EQ(1, batches);
  EXPECT_EQ(1u, queue.size());
  queue.pop_front();
  EXPECT_TRUE(queue.CanProduce());
  queue.Pump();
  EXPECT_EQ(2, batches);
  // Pending tasks capture local owners. Retire them before those owners die.
  while (!queue.empty()) queue.pop_front();
  tasks.RunUntilIdle();
}
TEST(GuestControlQueue, InterleavedTitlesCoalesceButReliableControlStaysOrdered) {
  ControlQueue queue;
  Owner first{1, 1, 1, 1}, second{2, 2, 2, 1};
  queue.Push({Message::kTitle, {}, 0, first, "old"});
  queue.Push({Message::kTitle, {}, 0, second, "other"});
  queue.Push({Message::kNavigationComplete, {}, 0, first, "200"});
  queue.Push({Message::kTitle, {}, 0, first, "latest"});
  EXPECT_EQ(3u, queue.size());
  EXPECT_EQ("latest", queue.front().payload);
  queue.pop_front();
  EXPECT_EQ("other", queue.front().payload);
  queue.pop_front();
  EXPECT_EQ(Message::kNavigationComplete, queue.front().message);
}
TEST(GuestControlQueue, PartiallySentFramesNeverChange) {
  ControlQueue queue;
  Owner owner{1, 1, 1, 1};
  queue.Push({Message::kTitle, {}, 0, owner, "in-flight"});
  queue.Push({Message::kTitle, {}, 0, owner, "new"}, true);
  queue.Push({Message::kTitle, {}, 0, owner, "newest"}, true);
  EXPECT_EQ(2u, queue.size());
  EXPECT_EQ("in-flight", queue.front().payload);
  queue.pop_front();
  EXPECT_EQ("newest", queue.front().payload);
}
TEST(GuestControlQueue, DrainedOutputReadsCommandsBeforeProducingAgain) {
  base::test::SingleThreadTaskEnvironment tasks;
  ControlQueue queue;
  Channel channel;
  int batches = 0;
  auto produce = [&] {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce([](ControlQueue* queue, int* batches) {
          ++*batches;
          queue->Push({Message::kTitle, {}, 0, {1, 1, 1, 1}, "busy"});
        }, base::Unretained(&queue), base::Unretained(&batches)));
  };
  produce();
  queue.Pump(channel.fds[0]);
  ASSERT_EQ(1, batches);
  queue.pop_front();
  produce();
  Peer app(Id()), host(Id());
  ASSERT_TRUE(app.Send(channel.fds[1], {Message::kClose, {}, 0, {}, {}}));
  queue.Pump(channel.fds[0]);
  EXPECT_EQ(1, batches);
  EXPECT_TRUE(queue.empty());
  Envelope command;
  ASSERT_TRUE(host.Receive(channel.fds[0], &command));
  EXPECT_EQ(Message::kClose, command.message);
  queue.Pump(channel.fds[0]);
  EXPECT_EQ(2, batches);
}
TEST(GuestProviders, UnsupportedNeverInvokesNativeFallback) {
  auto session = Active();
  auto owner = session.CreateView();
  Providers providers([&](const Owner& value) { return session.Owns(value); },
                       [](std::function<void()> task) { task(); });
  providers.Admit();
  for (auto feature : {Feature::kLinuxLock, Feature::kStream, Feature::kMedia,
                        Feature::kIntegration, Feature::kAccessibility,
                        Feature::kNamespaceLock, Feature::kCapture}) {
    EXPECT_FALSE(providers.Available(feature));
    int count = 0;
    providers.Invoke(feature, *owner, "fixture", {},
                      [&](ProviderResult result, std::string value) {
      ++count;
      EXPECT_EQ(ProviderResult::kUnavailable, result);
      EXPECT_TRUE(value.empty());
    });
    EXPECT_EQ(1, count);
  }
}
TEST(GuestProviders, LateAndDuplicateRepliesStayWithOriginalOwner) {
  auto session = Active();
  auto owner = session.CreateView();
  ProviderReply delayed;
  std::deque<std::function<void()>> posted;
  Providers providers([&](const Owner& value) { return session.Owns(value); },
                       [&](std::function<void()> task) { posted.push_back(std::move(task)); });
  ASSERT_TRUE(providers.Install(Feature::kStream, StreamProvider(
      [&](const Owner&, const std::string&, const std::string&, ProviderReply reply) {
    delayed = std::move(reply);
  })));
  providers.Admit();
  int replies = 0;
  providers.Invoke(Feature::kStream, *owner, "fixture", {},
      [&](ProviderResult result, std::string value) {
    ++replies;
    EXPECT_EQ(ProviderResult::kCancelled, result);
    EXPECT_TRUE(value.empty());
  });
  delayed(ProviderResult::kOk, "old pixels");
  delayed(ProviderResult::kOk, "duplicate");
  EXPECT_EQ(0, replies);
  session.Navigate(owner->view);
  while (!posted.empty()) {
    auto task = std::move(posted.front());
    posted.pop_front();
    task();
  }
  EXPECT_EQ(1, replies);
}
TEST(GuestProviders, CloseCancelsBeforeOwnerRetirementAndIsReentrant) {
  auto session = Active();
  auto owner = session.CreateView();
  ProviderReply delayed;
  auto providers = std::make_unique<Providers>(
      [&](const Owner& value) { return session.Owns(value); },
      [](std::function<void()> task) { task(); });
  ASSERT_TRUE(providers->Install(Feature::kCapture, CaptureProvider(
      [&](const Owner&, const std::string&, const std::string&, ProviderReply reply) {
    delayed = std::move(reply);
  })));
  providers->Admit();
  int replies = 0;
  providers->Invoke(Feature::kCapture, *owner, "fixture", {},
      [&](ProviderResult result, std::string) {
    ++replies;
    EXPECT_TRUE(session.Owns(*owner));
    EXPECT_EQ(ProviderResult::kCancelled, result);
    providers->Close();
  });
  providers->Close();
  session.BeginClose();
  providers.reset();
  delayed(ProviderResult::kOk, "too late");
  EXPECT_EQ(1, replies);
}
}  // namespace
}  // namespace alloy::guest
