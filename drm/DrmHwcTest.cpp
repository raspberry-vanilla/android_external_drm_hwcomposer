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

#include "drm/DrmHwc.h"

#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "compositor/HdcpController.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmTestUtils.h"
#include "drm/ResourceManager.h"
#include "hwc/HwcDisplay.h"

namespace android::drm_hwcomposer {

namespace {

constexpr uint32_t kSharedCrtcId = 20;
constexpr uint32_t kSharedPlaneId = 30;
constexpr uint32_t kFirstEncoderId = 10;
constexpr uint32_t kSecondEncoderId = 11;
constexpr uint32_t kFirstConnectorId = 100;
constexpr uint32_t kSecondConnectorId = 101;

class RecordingDrmHwc : public DrmHwc {
 public:
  bool HasCallback() const override {
    return true;
  }
  void SendVsyncEventToClient(DisplayHandle /*display_handle*/,
                              int64_t /*timestamp*/,
                              uint32_t /*vsync_period*/) const override {
  }
  void SendVsyncPeriodTimingChangedEventToClient(
      DisplayHandle /*display_handle*/, int64_t /*timestamp*/) const override {
  }
  void SendRefreshEventToClient(DisplayHandle /*display_handle*/) override {
  }
  void SendHotplugEventToClient(DisplayHandle display_handle,
                                DisplayStatus display_status) override {
    hotplug_events.emplace_back(display_handle, display_status);
  }
  void SendHdcpLevelsChangedEventToClient(
      DisplayHandle /*display_handle*/,
      std::optional<HdcpContentType> /*current_hdcp_level*/) override {
  }

  std::vector<std::pair<DisplayHandle, DisplayStatus>> hotplug_events;
};

}  // namespace

struct DrmHwcTest : public ::testing::Test {
  void SetUp() override {
    fake_device = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
    fake_device->GetFakeAtomicCommitSink()->SetDriveStateManager(true);
  }

  void TearDown() override {
    hwc.DeinitDisplays();
  }

  std::shared_ptr<DrmDisplayPipeline> CreatePipeline(
      bool is_external = false) const {
    fake_device->AddPipelineResources(is_external);
    return fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
  }

  void AddDeviceToResMan(std::unique_ptr<DrmDevice> dev) {
    hwc.GetResMan().drms_.emplace_back(std::move(dev));
  }

  void RunUpdateFrontendDisplays() {
    hwc.GetResMan().UpdateFrontendDisplays();
  }

  size_t GetAttachedPipelineCount(DrmConnector* conn) {
    return hwc.GetResMan().attached_pipelines_.count(conn);
  }

  void ResetResManDevices() {
    hwc.GetResMan().DetachAllFrontendDisplays();
    hwc.GetResMan().drms_.clear();
  }

  std::unique_ptr<FakeDrmDevice> fake_device;
  RecordingDrmHwc hwc;
};

TEST_F(DrmHwcTest, BindAndUnbindPrimaryDisplayTransitionsToHeadless) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_FALSE(primary->IsInHeadlessMode());
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));

  hwc.hotplug_events.clear();
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_TRUE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));
}

TEST_F(DrmHwcTest, BindAndUnbindExternalDisplayAlongsidePrimary) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  ASSERT_NE(hwc.GetDisplay(1), nullptr);
  EXPECT_FALSE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());
  EXPECT_FALSE(hwc.GetDisplay(1)->IsInHeadlessMode());
  ASSERT_EQ(hwc.hotplug_events.size(), 2U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));
  EXPECT_EQ(hwc.hotplug_events[1],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kConnected));

  // Unbinding the external display removes display 1 while keeping primary.
  hwc.hotplug_events.clear();
  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  EXPECT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_EQ(hwc.GetDisplay(1), nullptr);
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kDisconnected));

  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest, ActiveDisplayCountsAndDumpState) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  const auto counts = hwc.PullActiveDisplayCounts();
  EXPECT_EQ(counts.num_active_physical_displays, 2);
  EXPECT_EQ(counts.num_active_external_displays, 1);
  EXPECT_EQ(counts.num_virtual_displays, 0);
  EXPECT_FALSE(hwc.DumpState().empty());

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest, HotplugHdcpNegotiationStartsOnlyOnExternalSstDisplays) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);

  // Set up HDCP properties
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* external_connector = static_cast<FakeDrmConnector*>(
      external_pipe->connector->Get());
  external_connector->SetContentProtectionPropertySupported(true);
  external_connector->SetHdcpContentTypePropertySupported(true);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* internal_connector = static_cast<FakeDrmConnector*>(
      primary_pipe->connector->Get());
  internal_connector->SetContentProtectionPropertySupported(true);
  internal_connector->SetHdcpContentTypePropertySupported(true);

  // Bind internal display
  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  // Verify internal display skips HDCP
  auto* primary_display = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary_display, nullptr);
  const auto* internal_hdcp_controller = primary_display->GetHdcpController();
  ASSERT_NE(internal_hdcp_controller, nullptr);
  EXPECT_EQ(internal_hdcp_controller->GetHdcpState(),
            HdcpController::HdcpState::kUndesired);

  // Bind external display
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  // Verify external display starts HDCP negotiation
  auto* external_display = hwc.GetDisplay(1);
  ASSERT_NE(external_display, nullptr);
  const auto* external_hdcp_controller = external_display->GetHdcpController();
  ASSERT_NE(external_hdcp_controller, nullptr);
  EXPECT_EQ(external_hdcp_controller->GetHdcpState(),
            HdcpController::HdcpState::kDesired);

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest,
       UpdateFrontendDisplaysDetachesDisconnectedBeforeAttachingConnected) {
  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto* shared_dev_ptr = shared_dev.get();
  shared_dev->GetFakeAtomicCommitSink()->SetDriveStateManager(true);

  shared_dev->AddCrtc(std::make_unique<FakeDrmCrtc>(kSharedCrtcId));
  shared_dev->AddPlane(
      std::make_unique<FakeDrmPlane>(*shared_dev, DRM_PLANE_TYPE_PRIMARY,
                                     /*is_valid=*/true, kSharedPlaneId));
  shared_dev->AddEncoder(
      std::make_unique<FakeDrmEncoder>(kFirstEncoderId, kSharedCrtcId));
  shared_dev->AddEncoder(
      std::make_unique<FakeDrmEncoder>(kSecondEncoderId, kSharedCrtcId));

  auto conn0_uptr = std::make_unique<FakeDrmConnector>(shared_dev.get(),
                                                       kFirstEncoderId,
                                                       /*is_external=*/true,
                                                       kFakeConnectorMmWidth,
                                                       kFakeConnectorMmHeight,
                                                       kFirstConnectorId);
  conn0_uptr->SetModes({CreateFakeDrmMode()});
  conn0_uptr->SetConnected(false);
  auto* conn0 = conn0_uptr.get();
  shared_dev->AddConnector(std::move(conn0_uptr));

  auto conn1_uptr = std::make_unique<FakeDrmConnector>(shared_dev.get(),
                                                       kSecondEncoderId,
                                                       /*is_external=*/true,
                                                       kFakeConnectorMmWidth,
                                                       kFakeConnectorMmHeight,
                                                       kSecondConnectorId);
  conn1_uptr->SetModes({CreateFakeDrmMode()});
  conn1_uptr->SetConnected(true);
  auto* conn1 = conn1_uptr.get();
  shared_dev->AddConnector(std::move(conn1_uptr));

  AddDeviceToResMan(std::move(shared_dev));
  RunUpdateFrontendDisplays();
  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  EXPECT_EQ(GetAttachedPipelineCount(conn0), 0U);
  EXPECT_EQ(GetAttachedPipelineCount(conn1), 1U);

  // Lower-indexed conn0 connects in the same update pass that higher-indexed
  // conn1 disconnects. Detaching conn1 first frees the single shared CRTC so
  // conn0 can bind it.
  conn0->SetConnected(true);
  conn1->SetConnected(false);
  RunUpdateFrontendDisplays();

  EXPECT_EQ(GetAttachedPipelineCount(conn0), 1U);
  EXPECT_EQ(GetAttachedPipelineCount(conn1), 0U);
  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_FALSE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  ResetResManDevices();
}

TEST_F(DrmHwcTest,
       PrimaryDisconnectPromotesSecondaryAndDisconnectsOldHandleFirst) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();
  ASSERT_EQ(hwc.hotplug_events.size(), 2U);

  // Unbinding primary while external remains connected promotes external to
  // kPrimaryDisplay (handle 0). FlushHotplugEvents must dispatch
  // (1, kDisconnected) before (0, kConnected).
  hwc.hotplug_events.clear();
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  ASSERT_EQ(hwc.hotplug_events.size(), 2U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kDisconnected));
  EXPECT_EQ(hwc.hotplug_events[1],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
}

}  // namespace android::drm_hwcomposer
