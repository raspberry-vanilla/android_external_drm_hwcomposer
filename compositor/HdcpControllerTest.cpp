/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "compositor/HdcpController.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "compositor/DisplayInfo.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmTestUtils.h"

namespace android::drm_hwcomposer {

namespace {

class BlockingQueryDrmConnector : public FakeDrmConnector {
 public:
  using FakeDrmConnector::FakeDrmConnector;

  void SetQueryHook(std::function<bool()> hook) {
    std::lock_guard<std::mutex> lock(hook_mutex_);
    query_hook_ = std::move(hook);
  }

  bool QueryContentProtectionEnabled() override {
    std::function<bool()> hook;
    {
      std::lock_guard<std::mutex> lock(hook_mutex_);
      hook = query_hook_;
    }
    if (hook) {
      return hook();
    }
    return FakeDrmConnector::QueryContentProtectionEnabled();
  }

 private:
  std::mutex hook_mutex_;
  std::function<bool()> query_hook_;
};

}  // namespace

class HdcpControllerTest : public ::testing::Test {
 protected:
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  // NOLINTBEGIN(readability-identifier-naming)
  void SetUp() override {
    device_ = std::make_unique<FakeDrmDevice>();
    device_->AddPipelineResources(/*is_external=*/true);

    // Set up HDCP properties
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    connector_ = static_cast<FakeDrmConnector*>(
        device_->GetConnectors().back().get());
    connector_->SetContentProtectionPropertySupported(true);
    connector_->SetHdcpContentTypePropertySupported(true);

    pipeline_ = device_->GetBackend().CreatePipeline(*connector_);

    callbacks_ = {[this](std::optional<HdcpContentType> type) {
                    std::lock_guard<std::mutex> lock(cbk_mutex_);
                    last_notified_type_ = type;
                    notification_count_++;
                    cbk_cv_.notify_all();
                  },
                  [this]() {
                    std::lock_guard<std::mutex> lock(cbk_mutex_);
                    retry_frame_triggered_ = true;
                    cbk_cv_.notify_all();
                  }};
  }

  void TearDown() override {
    controller_.reset();
    pipeline_.reset();
    device_.reset();
  }

  static constexpr int kDefaultTimeoutMs = 10;
  void CreateController(std::chrono::milliseconds timeout =
                            std::chrono::milliseconds(kDefaultTimeoutMs)) {
    controller_ = std::make_unique<HdcpController>(pipeline_.get(), callbacks_,
                                                   timeout);
  }

  std::optional<HdcpContentType> WaitForNotification() {
    std::unique_lock<std::mutex> lock(cbk_mutex_);
    cbk_cv_.wait(lock, [this]() { return notification_count_ > 0; });
    notification_count_--;
    return last_notified_type_;
  }

  bool WaitForRetryFrame() {
    std::unique_lock<std::mutex> lock(cbk_mutex_);
    cbk_cv_.wait(lock, [this]() { return retry_frame_triggered_; });
    retry_frame_triggered_ = false;
    return true;
  }

  void ClearNotifications() {
    std::lock_guard<std::mutex> lock(cbk_mutex_);
    notification_count_ = 0;
    last_notified_type_ = std::nullopt;
    retry_frame_triggered_ = false;
  }

  std::unique_lock<std::mutex> LockCallbacks() {
    return std::unique_lock<std::mutex>(cbk_mutex_);
  }

  std::shared_ptr<DrmDisplayPipeline> pipeline_;
  std::unique_ptr<FakeDrmDevice> device_;
  FakeDrmConnector* connector_ = nullptr;
  std::unique_ptr<HdcpController> controller_;

  HdcpConCallbacks callbacks_;

  std::mutex cbk_mutex_;
  std::condition_variable cbk_cv_;
  std::optional<HdcpContentType> last_notified_type_ = std::nullopt;
  int notification_count_ = 0;
  bool retry_frame_triggered_ = false;
  // NOLINTEND(readability-identifier-naming)
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

TEST_F(HdcpControllerTest, InitialState) {
  CreateController();
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kUndesired);
}

TEST_F(HdcpControllerTest, Type1NegotiationSuccess) {
  CreateController();
  controller_->Start();
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kDesired);
  EXPECT_EQ(controller_->GetPendingContentType(), HdcpContentType::kType1);

  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();
  EXPECT_EQ(controller_->GetPendingContentType(), std::nullopt);

  auto type = WaitForNotification();
  EXPECT_EQ(type, HdcpContentType::kType1);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);
}

TEST_F(HdcpControllerTest, DirectType0NegotiationSuccess) {
  CreateController();
  controller_->Start(HdcpContentType::kType0);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kDesired);
  EXPECT_EQ(controller_->GetPendingContentType(), HdcpContentType::kType0);

  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();
  EXPECT_EQ(controller_->GetPendingContentType(), std::nullopt);

  auto type = WaitForNotification();
  EXPECT_EQ(type, HdcpContentType::kType0);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);
}

TEST_F(HdcpControllerTest, DirectType0FailureMarksInfeasibleWithoutRetry) {
  CreateController();
  controller_->Start(HdcpContentType::kType0);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kDesired);
  EXPECT_EQ(controller_->GetPendingContentType(), HdcpContentType::kType0);

  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();

  auto type = WaitForNotification();
  EXPECT_EQ(type, std::nullopt);
  EXPECT_EQ(controller_->GetHdcpState(),
            HdcpController::HdcpState::kInfeasible);
  EXPECT_EQ(controller_->GetPendingContentType(), std::nullopt);
  {
    auto cbk_lock = LockCallbacks();
    EXPECT_FALSE(retry_frame_triggered_);
  }
}

TEST_F(HdcpControllerTest, Type0RetryFallback) {
  CreateController();
  controller_->Start();
  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();

  EXPECT_TRUE(WaitForRetryFrame());
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRetry);

  // Retry frame requested
  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();

  auto type = WaitForNotification();
  EXPECT_EQ(type, HdcpContentType::kType0);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);
}

TEST_F(HdcpControllerTest, StartDuringRequestedPreservesInFlightNegotiation) {
  constexpr int kLongTimeoutMs = 5000;
  CreateController(std::chrono::milliseconds(kLongTimeoutMs));

  // Duplicate Start() while Type 1 authentication is in flight must not reset
  // kRequested back to kDesired or overwrite target_content_type_.
  controller_->Start(HdcpContentType::kType1);
  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();
  ASSERT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRequested);

  controller_->Start(HdcpContentType::kType0);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRequested);
  EXPECT_EQ(controller_->GetPendingContentType(), std::nullopt);

  connector_->SetContentProtectionEnabled(true);
  EXPECT_EQ(WaitForNotification(), HdcpContentType::kType1);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);

  // Also verify duplicate Start(kType1) during a Type 0 fallback's kRequested
  // window does not clobber target_content_type_ back to kType1.
  controller_->Terminate();
  ClearNotifications();
  connector_->SetContentProtectionEnabled(false);

  controller_->Start(HdcpContentType::kType1);
  controller_->Requested();
  controller_->ExpireRequestTimeoutForTesting();
  ASSERT_TRUE(WaitForRetryFrame());
  ASSERT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRetry);
  ASSERT_EQ(controller_->GetPendingContentType(), HdcpContentType::kType0);

  controller_->Requested();
  ASSERT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRequested);

  controller_->Start(HdcpContentType::kType1);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRequested);
  EXPECT_EQ(controller_->GetPendingContentType(), std::nullopt);

  connector_->SetContentProtectionEnabled(true);
  EXPECT_EQ(WaitForNotification(), HdcpContentType::kType0);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);
}

TEST_F(HdcpControllerTest, InfeasibleTerminationAfterRetryFailure) {
  CreateController();
  controller_->Start();
  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();

  EXPECT_TRUE(WaitForRetryFrame());
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRetry);

  // Retry frame requested, but fails again
  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();

  auto type = WaitForNotification();
  EXPECT_EQ(type, std::nullopt);
  EXPECT_EQ(controller_->GetHdcpState(),
            HdcpController::HdcpState::kInfeasible);
}

TEST_F(HdcpControllerTest, ImmediateClientStatusNotification) {
  CreateController();

  // Bring to kEnabled (Type 1)
  controller_->Start();
  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();
  WaitForNotification();

  ClearNotifications();
  controller_->Start();
  auto type = WaitForNotification();
  EXPECT_EQ(type, HdcpContentType::kType1);

  // Reset and bring to kInfeasible
  controller_->Terminate();
  ClearNotifications();

  controller_->Start();
  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();
  WaitForRetryFrame();

  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();
  WaitForNotification();  // Infeasible notification

  ClearNotifications();
  controller_->Start();
  type = WaitForNotification();
  EXPECT_EQ(type, std::nullopt);
}

TEST_F(HdcpControllerTest, StateResetOnTerminate) {
  CreateController();
  controller_->Start();
  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();
  WaitForNotification();

  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);

  controller_->Terminate();
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kUndesired);
}

TEST_F(HdcpControllerTest, ResetPreservesTerminalInfeasibleState) {
  CreateController();
  controller_->Start();
  connector_->SetContentProtectionEnabled(false);
  controller_->Requested();
  WaitForRetryFrame();
  controller_->Requested();
  WaitForNotification();

  ASSERT_EQ(controller_->GetHdcpState(),
            HdcpController::HdcpState::kInfeasible);
  std::optional<HdcpContentType> level;
  ASSERT_TRUE(controller_->GetSettledNotification(&level));
  EXPECT_EQ(level, std::nullopt);

  controller_->Reset();
  EXPECT_EQ(controller_->GetHdcpState(),
            HdcpController::HdcpState::kInfeasible);
  EXPECT_TRUE(controller_->GetSettledNotification(&level));
  EXPECT_EQ(level, std::nullopt);
}

TEST_F(HdcpControllerTest, ResetFromEnabledReturnsToUndesired) {
  CreateController();
  controller_->Start();
  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();
  EXPECT_EQ(WaitForNotification(), HdcpContentType::kType1);
  ASSERT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);

  controller_->Reset();
  std::optional<HdcpContentType> level;
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kUndesired);
  EXPECT_FALSE(controller_->GetSettledNotification(&level));
}

TEST_F(HdcpControllerTest,
       StaleConnectorQueryAcrossResetDoesNotCorruptNewRequest) {
  pipeline_.reset();
  auto blocking_conn = std::make_unique<
      BlockingQueryDrmConnector>(device_.get(),
                                 device_->GetEncoders().back()->GetId(),
                                 /*is_external=*/true, /*width=*/0,
                                 /*height=*/0, connector_->GetId() + 1);
  blocking_conn->SetContentProtectionPropertySupported(true);
  blocking_conn->SetHdcpContentTypePropertySupported(true);
  auto* conn_ptr = blocking_conn.get();
  device_->AddConnector(std::move(blocking_conn));
  pipeline_ = device_->GetBackend().CreatePipeline(*conn_ptr);
  ASSERT_NE(pipeline_, nullptr);

  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  int query_count = 0;
  bool release_first_query = false;
  conn_ptr->SetQueryHook([&]() {
    std::unique_lock<std::mutex> lock(sync_mutex);
    ++query_count;
    sync_cv.notify_all();
    if (query_count == 1) {
      sync_cv.wait(lock, [&]() { return release_first_query; });
      return true;
    }
    return false;
  });

  constexpr int kLongTimeoutMs = 5000;
  CreateController(std::chrono::milliseconds(kLongTimeoutMs));
  controller_->Start();
  controller_->Requested();

  {
    std::unique_lock<std::mutex> lock(sync_mutex);
    sync_cv.wait(lock, [&]() { return query_count >= 1; });
  }
  controller_->Reset();
  controller_->Start();
  controller_->Requested();
  {
    std::lock_guard<std::mutex> lock(sync_mutex);
    release_first_query = true;
  }
  sync_cv.notify_all();
  {
    std::unique_lock<std::mutex> lock(sync_mutex);
    sync_cv.wait(lock, [&]() { return query_count >= 2; });
  }

  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kRequested);
  {
    auto cbk_lock = LockCallbacks();
    EXPECT_EQ(notification_count_, 0);
  }
  controller_.reset();
  conn_ptr->SetQueryHook(nullptr);
}

}  // namespace android::drm_hwcomposer
