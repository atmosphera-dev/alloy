// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "third_party/blink/renderer/core/dom/permission_service_connection.h"

#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"

namespace blink {
namespace {
using Service = mojom::blink::PermissionService;
using Status = mojom::blink::PermissionStatus;
using Reply = mojom::blink::PermissionStatusWithDetailsPtr;

auto Descriptor() {
  auto value = mojom::blink::PermissionDescriptor::New();
  value->name = mojom::blink::PermissionName::GEOLOCATION;
  return value;
}

class PermissionConnectionLifecycleTest : public testing::Test {
 protected:
  void SetUp() override {
    BindPermissionServiceConnection(
        base::BindRepeating(&PermissionConnectionLifecycleTest::BindBrowser,
                            base::Unretained(this)),
        client_.BindNewPipeAndPassReceiver(),
        base::SequencedTaskRunner::GetCurrentDefault());
  }
  void BindBrowser(mojo::PendingReceiver<Service> receiver) {
    browser_.push_back(std::move(receiver));
    ++bindings_;
  }
  void Pump() { tasks_.RunUntilIdle(); }
  void CloseBrowser() { browser_.clear(); Pump(); }
  base::test::TaskEnvironment tasks_;
  mojo::Remote<Service> client_;
  std::vector<mojo::PendingReceiver<Service>> browser_;
  size_t bindings_ = 0;
};

TEST_F(PermissionConnectionLifecycleTest,
       ReceiverRetirementAnswersEveryQueuedOperationWithoutDispatch) {
  base::test::TestFuture<Reply> query, request, revoke;
  base::test::TestFuture<Vector<Reply>> bulk;
  base::test::TestFuture<mojom::blink::EmbeddedPermissionControlResult> embedded;
  client_->HasPermission(Descriptor(), query.GetCallback());
  client_->RequestPermission(Descriptor(), request.GetCallback());
  client_->RevokePermission(Descriptor(), revoke.GetCallback());
  Vector<mojom::blink::PermissionDescriptorPtr> descriptors;
  descriptors.push_back(Descriptor());
  descriptors.push_back(Descriptor());
  client_->RequestPermissions(std::move(descriptors), bulk.GetCallback());
  auto control = mojom::blink::EmbeddedPermissionRequestDescriptor::New();
  control->detail =
      mojom::blink::EmbeddedPermissionControlDescriptorExtension::NewGeolocation(
          mojom::blink::GeolocationEmbeddedPermissionRequestDescriptor::New());
  client_->RequestPageEmbeddedPermission({}, std::move(control),
                                          embedded.GetCallback());
  Pump();
  ASSERT_EQ(browser_.size(), 1u);
  EXPECT_FALSE(query.IsReady());
  EXPECT_FALSE(request.IsReady());
  EXPECT_FALSE(revoke.IsReady());
  EXPECT_FALSE(bulk.IsReady());
  EXPECT_FALSE(embedded.IsReady());
  // The actual native endpoint is closed BEFORE any method can be dispatched.
  CloseBrowser();
  ASSERT_TRUE(query.IsReady());
  ASSERT_TRUE(request.IsReady());
  ASSERT_TRUE(revoke.IsReady());
  ASSERT_TRUE(bulk.IsReady());
  ASSERT_TRUE(embedded.IsReady());
  EXPECT_EQ(query.Get()->status, Status::DENIED);
  EXPECT_EQ(request.Get()->status, Status::DENIED);
  EXPECT_EQ(revoke.Get()->status, Status::DENIED);
  ASSERT_EQ(bulk.Get().size(), 2u);
  for (const auto& reply : bulk.Get()) {
    EXPECT_EQ(reply->status, Status::DENIED);
  }
  EXPECT_EQ(embedded.Get(),
            mojom::blink::EmbeddedPermissionControlResult::kDenied);
}

TEST_F(PermissionConnectionLifecycleTest,
       FreshQueryUsesANewBrowserConnectionAfterRetirement) {
  base::test::TestFuture<Reply> first, next;
  client_->HasPermission(Descriptor(), first.GetCallback());
  Pump();
  ASSERT_EQ(browser_.size(), 1u);
  CloseBrowser();
  ASSERT_TRUE(first.IsReady());
  EXPECT_EQ(first.Get()->status, Status::DENIED);
  client_->HasPermission(Descriptor(), next.GetCallback());
  Pump();
  ASSERT_EQ(browser_.size(), 1u);
  EXPECT_EQ(bindings_, 2u);
  EXPECT_FALSE(next.IsReady());
  CloseBrowser();
  ASSERT_TRUE(next.IsReady());
  EXPECT_EQ(next.Get()->status, Status::DENIED);
}
}  // namespace
}  // namespace blink
