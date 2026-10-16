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
#include <memory>
#include <mutex>
#include <optional>

#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmTestUtils.h"

namespace android::drm_hwcomposer {

class HdcpControllerTest : public ::testing::Test {
 protected:
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,
  // readability-identifier-naming)
  void SetUp() override {
    device_ = std::make_unique<FakeDrmDevice>();
    device_->AddPipelineResources(/*is_external=*/true);

    // Set up HDCP properties
    connector_ = static_cast<
        FakeDrmConnector*>(  // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
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
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,
  // readability-identifier-naming)
};

TEST_F(HdcpControllerTest, InitialState) {
  CreateController();
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kUndesired);
}

TEST_F(HdcpControllerTest, Type1NegotiationSuccess) {
  CreateController();
  controller_->Start();
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kDesired);

  connector_->SetContentProtectionEnabled(true);
  controller_->Requested();

  auto type = WaitForNotification();
  EXPECT_EQ(type, HdcpContentType::kType1);
  EXPECT_EQ(controller_->GetHdcpState(), HdcpController::HdcpState::kEnabled);
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

}  // namespace android::drm_hwcomposer