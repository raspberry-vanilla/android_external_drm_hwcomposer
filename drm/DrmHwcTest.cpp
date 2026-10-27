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
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <xf86drmMode.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <android-base/file.h>
#ifdef ANDROID
#include <android/log.h>
#endif
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "backend/GenericBackend.h"
#include "compositor/DisplayInfo.h"
#include "compositor/HdcpController.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmMode.h"
#include "drm/DrmTestUtils.h"
#include "drm/ResourceManager.h"
#include "drm/UEventListener.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "utils/TestUtils.h"
#include "utils/UEvent.h"
#include "utils/fd.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {

constexpr uint32_t kSharedCrtcId = 20;
constexpr uint32_t kSharedPlaneId = 30;
constexpr uint32_t kFirstEncoderId = 10;
constexpr uint32_t kSecondEncoderId = 11;
constexpr uint32_t kFirstConnectorId = 100;
constexpr uint32_t kSecondConnectorId = 101;
constexpr uint32_t kTestAnimWidth = 4;
constexpr uint32_t kTestAnimHeight = 4;
constexpr uint32_t kTestAnimFps = 240;
constexpr uint32_t kTestRefresh60Hz = 60;
constexpr uint32_t kTestRefresh120Hz = 120;
constexpr size_t kTestBytesPerPixel = 4;
constexpr uint8_t kTestPixelByte = 0x33;

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
  static void WriteValidRawAnimationFile(int fd, uint32_t num_frames = 2) {
    struct PackedHeader {
      char magic[4];
      uint32_t width;
      uint32_t height;
      uint32_t fps;
      uint32_t num_frames;
      uint32_t format;
      uint32_t hold_frame;
      uint32_t hold_duration_ms;
    } __attribute__((packed));

    PackedHeader hdr{};
    std::memcpy(hdr.magic, "RAWF", sizeof(hdr.magic));
    hdr.width = kTestAnimWidth;
    hdr.height = kTestAnimHeight;
    hdr.fps = kTestAnimFps;
    hdr.num_frames = num_frames;
    hdr.format = DRM_FORMAT_ARGB8888;
    hdr.hold_frame = 0;
    hdr.hold_duration_ms = 0;

    std::vector<uint8_t> pixels(static_cast<size_t>(hdr.width) * hdr.height *
                                    kTestBytesPerPixel * num_frames,
                                kTestPixelByte);
    ASSERT_EQ(write(fd, &hdr, sizeof(hdr)),
              static_cast<ssize_t>(sizeof(hdr)));
    ASSERT_EQ(write(fd, pixels.data(), pixels.size()),
              static_cast<ssize_t>(pixels.size()));
  }

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
    return CreatePipeline(*fake_device->GetConnectors().back());
  }

  std::shared_ptr<DrmDisplayPipeline> CreatePipeline(DrmConnector& conn) const {
    return fake_device->GetBackend().CreatePipeline(conn);
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
    hwc.GetResMan().CancelDelayedEdidRecovery();
    hwc.GetResMan().DetachAllFrontendDisplays();
    hwc.GetResMan().drms_.clear();
  }

  std::shared_ptr<DrmDisplayPipeline> CreatePipelineWithModes(
      std::vector<DrmMode> modes, bool is_external = false) const {
    fake_device->AddPipelineResourcesWithModes(std::move(modes), is_external);
    return fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
  }

  ScopedTestProperty reset_bootanim_completed{"vendor.hwc.bootanim.completed",
                                              ""};
  void TriggerDelayedEdidRecovery() {
    hwc.GetResMan().MaybeScheduleDelayedEdidRecovery();
  }

  void WaitDelayedEdidRecovery() {
    constexpr int kPollSleepMs = 10;
    while (true) {
      {
        std::scoped_lock lock(hwc.GetResMan().edid_recovery_mutex_);
        if (!hwc.GetResMan().edid_recovery_pending_) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollSleepMs));
    }
    hwc.GetResMan().CancelDelayedEdidRecovery();
  }

  void InitResManForTest() {
    hwc.GetResMan().Init();
  }

  void StartUEventListenerForTest(std::unique_ptr<UEvent> uevent) {
    hwc.GetResMan().StartUEventListener(std::move(uevent));
  }

  bool InvokeHandleUEventHotplugForTest(
      const UEventListener::HotplugEvent& event) {
    return hwc.GetResMan().HandleUEventHotplug(event);
  }

  void DeInitResManForTest() {
    hwc.GetResMan().DeInit();
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

  ASSERT_NE(hwc.GetDisplay(1), nullptr);
  EXPECT_EQ(hwc.GetDisplay(1)->SetPowerMode(HwcDisplay::PowerMode::kOn),
            HwcDisplay::Error::kNone);

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

TEST_F(DrmHwcTest, EarlyBootAnimationBindStartWaitStopAndRebindLifecycle) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd, /*num_frames=*/2);

  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty hold_prop("vendor.hwc.drm.early_boot_hold_ms", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);
  EXPECT_FALSE(Properties::EarlyBootAnimationCompleted());

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  hwc.StartEarlyBootAnimation();
  hwc.WaitForCompletionAndStopEarlyBootAnimation();

  EXPECT_GE(sink->GetExecuteCommitCount(), 2U);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);

  ASSERT_TRUE(primary->CreateLayer(1));
  (void)primary->ValidateStagedComposition();
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);

  // Simulate UI restart or display rebind after early boot animation completed.
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();

  primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);

  sink->ClearHistory();
  hwc.StartEarlyBootAnimation();
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
}

TEST_F(DrmHwcTest,
       StartEarlyBootAnimationOnHeadlessPrimaryMarksCompletedAndStaysInactive) {
  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");

  hwc.FinalizeDisplayBinding();
  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  ASSERT_TRUE(primary->IsInHeadlessMode());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);

  hwc.StartEarlyBootAnimation();
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
}

TEST_F(DrmHwcTest,
       StartEarlyBootAnimationOnExternalPrimaryMarksCompletedAndStaysInactive) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd, /*num_frames=*/2);

  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty hold_prop("vendor.hwc.drm.early_boot_hold_ms", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(external_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_FALSE(primary->IsInHeadlessMode());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
  EXPECT_FALSE(Properties::EarlyBootAnimationCompleted());

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  hwc.StartEarlyBootAnimation();
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
}

TEST_F(DrmHwcTest, ReinitPrimaryDisplayBeforeStartPreservesBootAnimation) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd, /*num_frames=*/2);

  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty hold_prop("vendor.hwc.drm.early_boot_hold_ms", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);
  EXPECT_FALSE(Properties::EarlyBootAnimationCompleted());

  // Re-initializing primary before StartEarlyBootAnimation() must keep
  // boot_animation_ intact and must not mark vendor.hwc.bootanim.completed=1.
  primary->SetPipeline(primary_pipe);
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);
  EXPECT_FALSE(Properties::EarlyBootAnimationCompleted());

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  hwc.StartEarlyBootAnimation();
  hwc.WaitForCompletionAndStopEarlyBootAnimation();

  EXPECT_GE(sink->GetExecuteCommitCount(), 2U);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);
}

TEST_F(DrmHwcTest,
       StartEarlyBootAnimationContinuesWhenBestConfigSwitchFails) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd, /*num_frames=*/2);

  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty hold_prop("vendor.hwc.drm.early_boot_hold_ms", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  // Provide 60Hz preferred and 120Hz candidate so Start() attempts SetConfig().
  auto primary_pipe = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFakeDefaultModeWidth, kFakeDefaultModeHeight,
                         kTestRefresh60Hz, DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kFakeDefaultModeWidth, kFakeDefaultModeHeight,
                         kTestRefresh120Hz, 0, "1080p120")},
      /*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(primary->GetCurrentConfig(), nullptr);
  const ConfigId preferred_id = primary->GetCurrentConfig()->id;

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  // Fail only the first commit (SetConfig to 120Hz), then let frame commits
  // succeed on the existing 60Hz config.
  sink->SetExecuteCommitHook(
      [sink](
          const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&
              /*args*/) {
        sink->SetExecuteCommitHook(nullptr);
        return CommitStatusOr<
            std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
            CommitStatus::Failure(-EINVAL));
      });

  hwc.StartEarlyBootAnimation();
  hwc.WaitForCompletionAndStopEarlyBootAnimation();

  ASSERT_NE(primary->GetCurrentConfig(), nullptr);
  EXPECT_EQ(primary->GetCurrentConfig()->id, preferred_id);
  // 1 failed SetConfig commit + at least 2 presented animation frames.
  EXPECT_GE(sink->GetExecuteCommitCount(), 3U);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);
}

TEST_F(DrmHwcTest,
       SecondaryDisplayInitSetConfigFailurePreservesPrimaryBootAnimation) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd, /*num_frames=*/2);

  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty hold_prop("vendor.hwc.drm.early_boot_hold_ms", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto secondary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(secondary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  // Fail only the secondary display's initial SetConfig() commit during Init().
  sink->SetExecuteCommitHook(
      [sink](
          const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&
              /*args*/) {
        sink->SetExecuteCommitHook(nullptr);
        return CommitStatusOr<
            std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
            CommitStatus::Failure(-EINVAL));
      });

  EXPECT_TRUE(hwc.BindDisplay(secondary_pipe));
  hwc.FinalizeDisplayBinding();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);
  EXPECT_FALSE(Properties::EarlyBootAnimationCompleted());

  sink->ClearHistory();
  hwc.StartEarlyBootAnimation();
  hwc.WaitForCompletionAndStopEarlyBootAnimation();

  EXPECT_GE(sink->GetExecuteCommitCount(), 2U);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
  EXPECT_EQ(primary->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);
}

TEST_F(DrmHwcTest, ZeroModeExternalConnectorIsIgnoredDuringHotplug) {
  auto dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto* dev_ptr = dev.get();
  dev->GetFakeAtomicCommitSink()->SetDriveStateManager(true);

  dev->AddCrtc(std::make_unique<FakeDrmCrtc>(kSharedCrtcId));
  dev->AddPlane(std::make_unique<FakeDrmPlane>(*dev, DRM_PLANE_TYPE_PRIMARY,
                                               /*is_valid=*/true,
                                               kSharedPlaneId));
  dev->AddEncoder(
      std::make_unique<FakeDrmEncoder>(kFirstEncoderId, kSharedCrtcId));

  // External connector reports connected with 0 valid modes (e.g. transient DP
  // AUX / DDC timeout on initial HPD).
  auto
      ext_conn_uptr = std::make_unique<FakeDrmConnector>(dev.get(),
                                                         kFirstEncoderId,
                                                         /*is_external=*/true,
                                                         kFakeConnectorMmWidth,
                                                         kFakeConnectorMmHeight,
                                                         kFirstConnectorId);
  ext_conn_uptr->SetModes({});
  ext_conn_uptr->SetConnected(true);
  auto* ext_conn = ext_conn_uptr.get();
  dev->AddConnector(std::move(ext_conn_uptr));

  AddDeviceToResMan(std::move(dev));
  hwc.hotplug_events.clear();
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();

  // The 0-mode external connector must not be recorded in attached_pipelines_
  // or bound as a fake 1024x768 HEADLESS-MODE external display.
  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 0U);
  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_TRUE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());

  dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  ResetResManDevices();
}

TEST_F(DrmHwcTest, ExternalConnectorBindsOnRetryAfterZeroModeHotplug) {
  auto dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto* dev_ptr = dev.get();
  dev->GetFakeAtomicCommitSink()->SetDriveStateManager(true);

  dev->AddCrtc(std::make_unique<FakeDrmCrtc>(kSharedCrtcId));
  dev->AddPlane(std::make_unique<FakeDrmPlane>(*dev, DRM_PLANE_TYPE_PRIMARY,
                                               /*is_valid=*/true,
                                               kSharedPlaneId));
  dev->AddEncoder(
      std::make_unique<FakeDrmEncoder>(kFirstEncoderId, kSharedCrtcId));

  auto
      ext_conn_uptr = std::make_unique<FakeDrmConnector>(dev.get(),
                                                         kFirstEncoderId,
                                                         /*is_external=*/true,
                                                         kFakeConnectorMmWidth,
                                                         kFakeConnectorMmHeight,
                                                         kFirstConnectorId);
  ext_conn_uptr->SetModes({});
  ext_conn_uptr->SetConnected(true);
  auto* ext_conn = ext_conn_uptr.get();
  dev->AddConnector(std::move(ext_conn_uptr));

  AddDeviceToResMan(std::move(dev));
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();
  ASSERT_EQ(GetAttachedPipelineCount(ext_conn), 0U);

  // When a follow-up hotplug arrives after EDID/mode probing succeeds,
  // ResourceManager must bind the connector and expose its real mode.
  ext_conn->SetModes({CreateFakeDrmMode()});
  hwc.hotplug_events.clear();
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();

  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 1U);
  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_FALSE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());
  EXPECT_EQ(hwc.GetDisplay(kPrimaryDisplay)->GetSize(),
            std::make_pair(uint32_t{kFakeDefaultModeWidth},
                           uint32_t{kFakeDefaultModeHeight}));

  dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  ResetResManDevices();
}

TEST_F(DrmHwcTest, HotplugEventQueuePreservesConnectBeforeLinkTrainingFailed) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);

  // Simulate ResourceManager::ProcessHotplugForConnector attaching an external
  // display whose link-status is immediately BAD in the same hotplug pass:
  // BindDisplay() queues (1, kConnected) and NotifyDisplayLinkStatus() queues
  // (1, kLinkTrainingFailed) before FlushHotplugEvents() runs.
  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.NotifyDisplayLinkStatus(external_pipe);
  hwc.FlushHotplugEvents();

  ASSERT_EQ(hwc.hotplug_events.size(), 3U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));
  EXPECT_EQ(hwc.hotplug_events[1],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kConnected));
  EXPECT_EQ(hwc.hotplug_events[2],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kLinkTrainingFailed));

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest,
       HotplugEventQueuePreservesDisconnectBeforeReconnectOnSameHandle) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);
  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();
  hwc.hotplug_events.clear();

  // Queue kDisconnected followed by kConnected for the same handle before
  // FlushHotplugEvents() runs. Both transitions must be delivered in order.
  hwc.ScheduleHotplugEvent(kPrimaryDisplay,
                           DrmHwc::DisplayStatus::kDisconnected);
  hwc.ScheduleHotplugEvent(kPrimaryDisplay, DrmHwc::DisplayStatus::kConnected);
  hwc.FlushHotplugEvents();

  ASSERT_EQ(hwc.hotplug_events.size(), 2U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kDisconnected));
  EXPECT_EQ(hwc.hotplug_events[1],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));

  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest,
       HotplugEventQueueDoesNotInvertConnectBeforeDisconnectOnSameHandle) {
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(external_pipe, nullptr);
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();
  hwc.hotplug_events.clear();

  // Queue kConnected followed by kDisconnected for the same handle before
  // FlushHotplugEvents() runs. The stale kConnected entry must be dropped so
  // FlushHotplugEvents() does not sort kDisconnected ahead of kConnected and
  // report a phantom reconnect for a disconnected display.
  hwc.ScheduleHotplugEvent(kPrimaryDisplay, DrmHwc::DisplayStatus::kConnected);
  hwc.ScheduleHotplugEvent(kPrimaryDisplay,
                           DrmHwc::DisplayStatus::kDisconnected);
  hwc.FlushHotplugEvents();

  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kDisconnected));

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest, DelayedEdidRecoveryFlushesHotplugEventsToClient) {
  const ScopedTestProperty recovery_delay("vendor.hwc.delayed_edid_recovery_ms",
                                          "10");
  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto* shared_dev_ptr = shared_dev.get();
  shared_dev->AddPipelineResources(/*is_external=*/false);
  auto* ext_conn = shared_dev->AddPipelineResources(/*is_external=*/true);

  // Configure the external connector with a valid 128-byte EDID v1.3 block
  // whose manufacturer ID is "NVT" (0x3A, 0xD4), which matches
  // HasDelayedEdidQuirk().
  constexpr size_t kEdidSize = 128;
  constexpr size_t kEdidHeaderSize = 8;
  constexpr size_t kEdidMfgByte0Index = 8;
  constexpr size_t kEdidMfgByte1Index = 9;
  constexpr size_t kEdidVersionIndex = 18;
  constexpr size_t kEdidRevisionIndex = 19;
  constexpr size_t kEdidChecksumIndex = 127;
  constexpr uint8_t kNvtMfgByte0 = 0x3A;
  constexpr uint8_t kNvtMfgByte1 = 0xD4;
  constexpr uint8_t kEdidVersion1 = 1;
  constexpr uint8_t kEdidRevision3 = 3;
  std::vector<uint8_t> nvt_edid(kEdidSize, 0);
  constexpr uint8_t kEdidHeader[kEdidHeaderSize] = {0x00, 0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF, 0x00};
  std::memcpy(nvt_edid.data(), kEdidHeader, sizeof(kEdidHeader));
  nvt_edid[kEdidMfgByte0Index] = kNvtMfgByte0;
  nvt_edid[kEdidMfgByte1Index] = kNvtMfgByte1;
  nvt_edid[kEdidVersionIndex] = kEdidVersion1;
  nvt_edid[kEdidRevisionIndex] = kEdidRevision3;
  uint8_t sum = 0;
  for (size_t i = 0; i < kEdidChecksumIndex; ++i) {
    sum = static_cast<uint8_t>(sum + nvt_edid[i]);
  }
  nvt_edid[kEdidChecksumIndex] = static_cast<uint8_t>(0U - sum);
  ext_conn->SetEdidBlobData(std::move(nvt_edid));

  AddDeviceToResMan(std::move(shared_dev));
  RunUpdateFrontendDisplays();
  // Clear FakeAtomicCommitSink history so its stored AtomicCommitArgs copies do
  // not hold a shared_ptr reference to BindingOwner<DrmPlane> across unbind.
  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  hwc.FlushHotplugEvents();
  ASSERT_EQ(hwc.hotplug_events.size(), 2U);
  hwc.hotplug_events.clear();

  TriggerDelayedEdidRecovery();
  WaitDelayedEdidRecovery();

  // MaybeScheduleDelayedEdidRecovery() must flush the queued kDisconnected and
  // kConnected events to the HWC client on its own when the recovery thread
  // finishes rebinding the display.
  EXPECT_EQ(hwc.hotplug_events.size(), 2U);
  if (hwc.hotplug_events.size() >= 2U) {
    EXPECT_EQ(hwc.hotplug_events[0].second,
              DrmHwc::DisplayStatus::kDisconnected);
    EXPECT_EQ(hwc.hotplug_events[1].second, DrmHwc::DisplayStatus::kConnected);
  }

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  ResetResManDevices();
}

namespace {

std::string MakeUEventPacket(std::initializer_list<std::string_view> fields) {
  std::string packet;
  for (const auto field : fields) {
    packet.append(field.data(), field.size());
    packet.push_back('\0');
  }
  return packet;
}

class BackendHotplugTestBackend : public GenericBackend {
 public:
  explicit BackendHotplugTestBackend(DrmDevice& drm) : GenericBackend(drm) {
  }

  bool UseBackendHotplug() const override {
    return true;
  }

  void SetHotplugHandler(HotplugHandler handler) override {
    hotplug_handler_ = std::move(handler);
  }

  void TriggerHotplug(uint32_t connector_id, bool connected) {
    if (hotplug_handler_) {
      hotplug_handler_(connector_id, connected);
    }
  }

 private:
  HotplugHandler hotplug_handler_;
};

}  // namespace

TEST_F(DrmHwcTest,
       BackendHotplugDisconnectDetachesDisplayEvenBeforeKernelStatusUpdates) {
  const ScopedTestProperty device_prop("vendor.hwc.drm.device",
                                       "/nonexistent_drm_device");

  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto backend = std::make_unique<BackendHotplugTestBackend>(*shared_dev);
  auto* backend_ptr = backend.get();
  shared_dev->SetBackend(std::move(backend));

  shared_dev->AddPipelineResources(/*is_external=*/false);
  shared_dev->AddPipelineResources(/*is_external=*/true);
  DrmConnector* primary_conn = shared_dev->GetConnectors()[0].get();
  DrmConnector* ext_conn = shared_dev->GetConnectors()[1].get();
  const uint32_t primary_id = primary_conn->GetId();
  const uint32_t ext_id = ext_conn->GetId();

  AddDeviceToResMan(std::move(shared_dev));
  InitResManForTest();

  backend_ptr->TriggerHotplug(primary_id, /*connected=*/true);
  backend_ptr->TriggerHotplug(ext_id, /*connected=*/true);
  ASSERT_EQ(GetAttachedPipelineCount(primary_conn), 1U);
  ASSERT_EQ(GetAttachedPipelineCount(ext_conn), 1U);
  hwc.hotplug_events.clear();

  // Backend reports connected = false for ext_id while ext_conn->IsConnected()
  // in DRM has not yet flipped to false. ResourceManager's backend hotplug
  // handler must honor connected == false and detach ext_conn.
  backend_ptr->TriggerHotplug(ext_id, /*connected=*/false);

  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 0U);
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kDisconnected));

  DeInitResManForTest();
}

TEST_F(DrmHwcTest,
       UEventReadNextConvertsTrailingNullByteAndSupportsLargePackets) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds), 0);
  auto uevent = UEvent::CreateInstanceWithFd(MakeUniqueFd(fds[0]));
  UniqueFd write_fd = MakeUniqueFd(fds[1]);
  ASSERT_NE(uevent, nullptr);

  // 1. Standard kernel uevent packet ends with a trailing '\0' byte.
  const std::string short_packet = MakeUEventPacket({
      "change@/devices/pci0000:00/drm/card0",
      "ACTION=change",
      "DEVTYPE=drm_minor",
      "HOTPLUG=1",
      "CONNECTOR=120",
  });
  ASSERT_EQ(write(*write_fd, short_packet.data(), short_packet.size()),
            static_cast<ssize_t>(short_packet.size()));

  auto parsed_short = uevent->ReadNext();
  ASSERT_TRUE(parsed_short.has_value());
  if (!parsed_short) {
    return;
  }
  EXPECT_EQ(parsed_short->find('\0'), std::string::npos)
      << "UEvent::ReadNext() left an un-converted trailing NUL byte in string";

  // 2. Large kernel uevent packet (> 1024 bytes, e.g. long PCIe/USB4 DEVPATH)
  // with HOTPLUG=1 near the end of the packet.
  constexpr size_t kLongDevpathPaddingLen = 1100;
  constexpr size_t kLegacyBufferLimit = 1024U;
  const std::string large_header = "change@" +
                                   std::string(kLongDevpathPaddingLen, 'a');
  const std::string large_packet = MakeUEventPacket({
      large_header,
      "ACTION=change",
      "DEVTYPE=drm_minor",
      "HOTPLUG=1",
  });
  ASSERT_GT(large_packet.size(), kLegacyBufferLimit);
  ASSERT_EQ(write(*write_fd, large_packet.data(), large_packet.size()),
            static_cast<ssize_t>(large_packet.size()));

  auto parsed_large = uevent->ReadNext();
  ASSERT_TRUE(parsed_large.has_value());
  if (!parsed_large) {
    return;
  }
  EXPECT_NE(parsed_large->find("HOTPLUG=1"), std::string::npos)
      << "UEvent::ReadNext() truncated a >1024-byte netlink uevent packet";
}

TEST_F(DrmHwcTest, UEventListenerDoesNotSwallowRapidUnplugReplugWithin200ms) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds), 0);
  auto uevent = UEvent::CreateInstanceWithFd(MakeUniqueFd(fds[0]));
  UniqueFd write_fd = MakeUniqueFd(fds[1]);
  ASSERT_NE(uevent, nullptr);

  std::atomic<bool> connector_connected{true};
  std::atomic<int> observed_disconnects{0};
  std::atomic<int> observed_connects{0};

  auto listener = UEventListener::CreateInstance(
      [&]() {
        if (connector_connected.load()) {
          ++observed_connects;
        } else {
          ++observed_disconnects;
        }
      },
      std::move(uevent));
  ASSERT_NE(listener, nullptr);

  constexpr int kRapidHotplugGapMs = 50;
  const std::string hotplug_uevent = MakeUEventPacket({
      "change@/devices/drm/card0",
      "DEVTYPE=drm_minor",
      "HOTPLUG=1",
  });

  // Unplug at t = 0ms, then replug 50ms later (< 200ms).
  connector_connected.store(false);
  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(kRapidHotplugGapMs));

  connector_connected.store(true);
  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(kRapidHotplugGapMs));
  listener.reset();

  EXPECT_GE(observed_disconnects.load(), 1)
      << "UEventListener 200ms sleep swallowed the sub-200ms unplug state";
  EXPECT_GE(observed_connects.load(), 1);
}

TEST_F(DrmHwcTest,
       UEventListenerRunsSettleRetryOnlyWhenHandlerReportsUnsettledConnector) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds), 0);
  auto uevent = UEvent::CreateInstanceWithFd(MakeUniqueFd(fds[0]));
  UniqueFd write_fd = MakeUniqueFd(fds[1]);
  ASSERT_NE(uevent, nullptr);

  std::atomic<bool> report_unsettled{false};
  std::atomic<int> handler_calls{0};
  auto listener = UEventListener::CreateInstance(
      [&]() -> bool {
        ++handler_calls;
        return report_unsettled.exchange(false);
      },
      std::move(uevent));
  ASSERT_NE(listener, nullptr);

  constexpr int kPostSettleWaitMs = 250;
  const std::string hotplug_uevent = MakeUEventPacket({
      "change@/devices/drm/card0",
      "DEVTYPE=drm_minor",
      "HOTPLUG=1",
  });

  // 1. When all connectors are settled (handler returns false), no 200ms
  // settle retry should run.
  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kPostSettleWaitMs));
  EXPECT_EQ(handler_calls.load(), 1);

  // 2. When a connector is still unsettled (handler returns true on first
  // pass), UEventListener must run the post-200ms settle retry.
  handler_calls.store(0);
  report_unsettled.store(true);
  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kPostSettleWaitMs));
  listener.reset();
  EXPECT_EQ(handler_calls.load(), 2);
}

TEST_F(DrmHwcTest,
       RefreshConnectorsAddsNewDynamicConnectorsAndReturnsStaleConnectors) {
  constexpr uint32_t kMstConnId1 = 200;
  constexpr uint32_t kMstConnId2 = 201;
  fake_device->AddPipelineResources(/*is_external=*/false);
  fake_device->AddPipelineResources(/*is_external=*/true);
  const uint32_t static_id_0 = fake_device->GetConnectors()[0]->GetId();
  const uint32_t static_id_1 = fake_device->GetConnectors()[1]->GetId();

  // Dynamic MST connectors 200 and 201 appear in DRM resources.
  fake_device->SetResourceConnectorIds(
      {static_id_0, static_id_1, kMstConnId1, kMstConnId2});
  {
    auto stale = fake_device->RefreshConnectors();
    EXPECT_TRUE(stale.empty());
  }
  ASSERT_EQ(fake_device->GetConnectors().size(), 4U);
  EXPECT_EQ(fake_device->GetConnectors()[2]->GetId(), kMstConnId1);
  EXPECT_EQ(fake_device->GetConnectors()[3]->GetId(), kMstConnId2);

  // Now MST connector 200 is unplugged and removed from DRM resources while 201
  // remains present.
  fake_device->SetResourceConnectorIds({static_id_0, static_id_1, kMstConnId2});
  {
    auto stale = fake_device->RefreshConnectors();
    ASSERT_EQ(stale.size(), 1U);
    EXPECT_EQ(stale[0]->GetId(), kMstConnId1);
  }
  ASSERT_EQ(fake_device->GetConnectors().size(), 3U);
  EXPECT_EQ(fake_device->GetConnectors()[2]->GetId(), kMstConnId2);
}

TEST_F(DrmHwcTest,
       NotifyDisplayLinkStatusRefreshesPrunedConfigsAndRemapsActiveConfig) {
  constexpr uint16_t kUhdWidth = 3840;
  constexpr uint16_t kUhdHeight = 2160;
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto* ext_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kUhdWidth, kUhdHeight, kFakeDefaultModeVRefresh,
                         DRM_MODE_TYPE_PREFERRED, "2160p60"),
       CreateFakeDrmMode(kFakeDefaultModeWidth, kFakeDefaultModeHeight,
                         kFakeDefaultModeVRefresh, 0, "1080p60")},
      /*is_external=*/true);
  std::shared_ptr<DrmDisplayPipeline> external_pipe = CreatePipeline(*ext_conn);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);
  ASSERT_NE(ext_conn, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  HwcDisplay* ext_display = hwc.GetDisplay(1);
  ASSERT_NE(ext_display, nullptr);
  EXPECT_EQ(ext_display->SetPowerMode(HwcDisplay::PowerMode::kOn),
            HwcDisplay::Error::kNone);
  const auto initial_configs = ext_display->GetDisplayConfigs();
  ASSERT_EQ(initial_configs.size(), 2U);
  ASSERT_NE(ext_display->GetCurrentConfig(), nullptr);
  ASSERT_NE(ext_display->GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(ext_display->GetLastRequestedConfig()->mode.GetRawMode().hdisplay,
            kUhdWidth);

  // Simulate kernel link-status = BAD pruning the 4K mode so only 1080p60
  // remains on the connector.
  ext_conn->SetModes(
      {CreateFakeDrmMode(kFakeDefaultModeWidth, kFakeDefaultModeHeight,
                         kFakeDefaultModeVRefresh, DRM_MODE_TYPE_PREFERRED,
                         "1080p60")});

  hwc.hotplug_events.clear();
  fake_device->GetFakeAtomicCommitSink()->ClearHistory();

  hwc.NotifyDisplayLinkStatus(external_pipe);
  hwc.FlushHotplugEvents();

  const auto refreshed_configs = ext_display->GetDisplayConfigs();
  EXPECT_EQ(refreshed_configs.size(), 1U)
      << "NotifyDisplayLinkStatus() did not refresh HwcDisplay::configs_ from "
         "the connector's pruned mode list";
  EXPECT_EQ(ext_display->GetCurrentConfig(), nullptr)
      << "GetCurrentConfig() must be invalidated until SetConfig() commits the "
         "pruned mode";
  ASSERT_NE(ext_display->GetLastRequestedConfig(), nullptr)
      << "GetLastRequestedConfig() returned nullptr after link-status config "
         "refresh";
  if (refreshed_configs.size() == 1U) {
    EXPECT_EQ(ext_display->GetLastRequestedConfig()->id,
              refreshed_configs[0].id);
    EXPECT_EQ(ext_display->GetLastRequestedConfig()->mode.GetRawMode().hdisplay,
              kFakeDefaultModeWidth);
  }
  EXPECT_EQ(fake_device->GetFakeAtomicCommitSink()->GetExecuteCommitCount(), 0U)
      << "NotifyDisplayLinkStatus() must not perform an autonomous KMS modeset";
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kLinkTrainingFailed));

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest,
       NotifyDisplayLinkStatusEmitsIncompatibleCableWhenAllModesPruned) {
  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto* shared_dev_ptr = shared_dev.get();
  shared_dev->AddPipelineResources(/*is_external=*/false);
  auto* ext_conn = shared_dev->AddPipelineResources(/*is_external=*/true);
  ASSERT_NE(ext_conn, nullptr);

  AddDeviceToResMan(std::move(shared_dev));
  RunUpdateFrontendDisplays();
  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  hwc.FlushHotplugEvents();
  ASSERT_EQ(GetAttachedPipelineCount(ext_conn), 1U);
  ASSERT_NE(hwc.GetDisplay(1), nullptr);

  // Simulate terminal link training failure where the kernel prunes all modes
  // on the connector and sets link-status = BAD while the cable remains plugged
  // in.
  ext_conn->SetModes({});
  ext_conn->SetLinkStatusPropertyValue(DRM_MODE_LINK_STATUS_BAD);

  hwc.hotplug_events.clear();
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();

  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 1U);
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kIncompatibleCable));

  // A subsequent hotplug sweep while still 0-mode + link-status = BAD must not
  // emit duplicate kIncompatibleCable events.
  hwc.hotplug_events.clear();
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();
  EXPECT_TRUE(hwc.hotplug_events.empty());

  // Also verify initial plug-in with 0 modes + link-status = BAD emits
  // kIncompatibleCable once without attaching a pipeline.
  hwc.hotplug_events.clear();
  ext_conn->SetConnected(false);
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();
  hwc.hotplug_events.clear();

  ext_conn->SetConnected(true);
  ext_conn->SetModes({});
  ext_conn->SetLinkStatusPropertyValue(DRM_MODE_LINK_STATUS_BAD);
  RunUpdateFrontendDisplays();
  hwc.FlushHotplugEvents();
  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 0U);
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0].second,
            DrmHwc::DisplayStatus::kIncompatibleCable);

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  ResetResManDevices();
}

TEST_F(DrmHwcTest,
       ResourceManagerInitRefreshesDynamicMstConnectorsBeforeBindingDisplays) {
  const ScopedTestProperty device_prop("vendor.hwc.drm.device",
                                       "/nonexistent_drm_device");
  constexpr uint32_t kDynamicMstConnId = 250;
  constexpr uint32_t kDynamicMstCrtcId = 251;
  constexpr uint32_t kDynamicMstPlaneId = 252;

  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  shared_dev->GetFakeAtomicCommitSink()->SetDriveStateManager(true);

  // Initial DrmDevice::Init() only captured the internal panel and the 1st MST
  // connector before the kernel's asynchronous MST LINK_ADDRESS worker finished
  // registering the 2nd MST connector (kDynamicMstConnId).
  shared_dev->AddPipelineResources(/*is_external=*/false);
  shared_dev->AddPipelineResources(/*is_external=*/true);
  const uint32_t primary_id = shared_dev->GetConnectors()[0]->GetId();
  const uint32_t first_mst_id = shared_dev->GetConnectors()[1]->GetId();

  // Provide an available encoder/CRTC/plane for the 2nd MST connector and
  // expose kDynamicMstConnId in drmModeGetResources() before
  // ResourceManager::Init() runs UpdateFrontendDisplays().
  shared_dev->AddEncoder(
      std::make_unique<FakeDrmEncoder>(kDynamicMstConnId, kDynamicMstCrtcId));
  shared_dev->AddCrtc(std::make_unique<FakeDrmCrtc>(kDynamicMstCrtcId));
  shared_dev->AddPlane(
      std::make_unique<FakeDrmPlane>(*shared_dev, DRM_PLANE_TYPE_PRIMARY,
                                     /*is_valid=*/true, kDynamicMstPlaneId));
  shared_dev->SetResourceConnectorIds(
      {primary_id, first_mst_id, kDynamicMstConnId});

  auto* shared_dev_ptr = shared_dev.get();
  AddDeviceToResMan(std::move(shared_dev));

  InitResManForTest();

  EXPECT_EQ(shared_dev_ptr->GetConnectors().size(), 3U)
      << "ResourceManager::Init() failed to call RefreshConnectors() before "
         "UpdateFrontendDisplays(), dropping the boot-connected MST connector";
  EXPECT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_NE(hwc.GetDisplay(1), nullptr);
  EXPECT_NE(hwc.GetDisplay(2), nullptr)
      << "2nd external MST display (kDynamicMstConnId) was not bound during "
         "ResourceManager::Init()";

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  DeInitResManForTest();
}

TEST_F(DrmHwcTest,
       UEventListenerTriggersHotplugOnDrmConnectorAddAndRemoveUevents) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds), 0);
  auto uevent = UEvent::CreateInstanceWithFd(MakeUniqueFd(fds[0]));
  UniqueFd write_fd = MakeUniqueFd(fds[1]);
  ASSERT_NE(uevent, nullptr);

  std::atomic<int> hotplug_callbacks{0};
  auto listener = UEventListener::CreateInstance([&]() { ++hotplug_callbacks; },
                                                 std::move(uevent));
  ASSERT_NE(listener, nullptr);

  constexpr int kDeliveryWaitMs = 50;
  const std::string connector_add_uevent = MakeUEventPacket({
      "add@/devices/pci0000:00/drm/card0/card0-DP-5",
      "ACTION=add",
      "DEVTYPE=drm_connector",
      "SUBSYSTEM=drm",
  });

  ASSERT_EQ(write(*write_fd, connector_add_uevent.data(),
                  connector_add_uevent.size()),
            static_cast<ssize_t>(connector_add_uevent.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kDeliveryWaitMs));
  EXPECT_GE(hotplug_callbacks.load(), 1)
      << "UEventListener ignored DEVTYPE=drm_connector ACTION=add uevent";

  const int after_add = hotplug_callbacks.load();
  const std::string connector_remove_uevent = MakeUEventPacket({
      "remove@/devices/pci0000:00/drm/card0/card0-DP-5",
      "ACTION=remove",
      "DEVTYPE=drm_connector",
      "SUBSYSTEM=drm",
  });

  ASSERT_EQ(write(*write_fd, connector_remove_uevent.data(),
                  connector_remove_uevent.size()),
            static_cast<ssize_t>(connector_remove_uevent.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kDeliveryWaitMs));
  listener.reset();

  EXPECT_GT(hotplug_callbacks.load(), after_add)
      << "UEventListener ignored DEVTYPE=drm_connector ACTION=remove uevent";
}

namespace {

class ScopedErrorLogCapture {
 public:
  ScopedErrorLogCapture() {
#ifdef ANDROID
    GetCapturedErrors().clear();
    __android_log_set_logger([](const struct __android_log_message* msg) {
      if (msg != nullptr && msg->priority >= ANDROID_LOG_ERROR &&
          msg->message != nullptr) {
        std::lock_guard<std::mutex> lock(GetMutex());
        GetCapturedErrors() += msg->message;
        GetCapturedErrors() += "\n";
      }
    });
#else
    testing::internal::CaptureStderr();
#endif
  }

  ~ScopedErrorLogCapture() {
    if (!active_) {
      return;
    }
    active_ = false;
#ifdef ANDROID
#ifdef __ANDROID__
    __android_log_set_logger(__android_log_logd_logger);
#else
    __android_log_set_logger(__android_log_stderr_logger);
#endif
#else
    (void)testing::internal::GetCapturedStderr();
#endif
  }

  ScopedErrorLogCapture(const ScopedErrorLogCapture&) = delete;
  ScopedErrorLogCapture& operator=(const ScopedErrorLogCapture&) = delete;
  ScopedErrorLogCapture(ScopedErrorLogCapture&&) = delete;
  ScopedErrorLogCapture& operator=(ScopedErrorLogCapture&&) = delete;

  std::string StopAndGetErrors() {
    if (!active_) {
      return {};
    }
    active_ = false;
#ifdef ANDROID
#ifdef __ANDROID__
    __android_log_set_logger(__android_log_logd_logger);
#else
    __android_log_set_logger(__android_log_stderr_logger);
#endif
    std::lock_guard<std::mutex> lock(GetMutex());
    std::string result = GetCapturedErrors();
    GetCapturedErrors().clear();
    return result;
#else
    return testing::internal::GetCapturedStderr();
#endif
  }

 private:
#ifdef ANDROID
  static std::mutex& GetMutex() {
    static auto* m = new std::mutex();
    return *m;
  }
  static std::string& GetCapturedErrors() {
    static auto* s = new std::string();
    return *s;
  }
#endif
  bool active_ = true;
};

}  // namespace

TEST_F(DrmHwcTest,
       TransientConnectorVanishSuppressesErrorLogsWhileRealIoErrorLogsError) {
  constexpr uint32_t kVanishedMstConnId = 310;
  constexpr uint32_t kMidInitVanishedMstConnId = 311;
  constexpr uint32_t kFaultyConnId = 312;
  constexpr uint32_t kPostCrtcIdVanishedMstConnId = 313;

  // 1. CreateInstance on a connector that vanished (-ENOENT) before
  // drmModeGetConnector() must return nullptr without emitting ALOGE.
  fake_device->SetGetConnectorFailureErrno(kVanishedMstConnId, ENOENT);
  {
    ScopedErrorLogCapture capture;
    auto conn = DrmConnector::CreateInstance(*fake_device, kVanishedMstConnId,
                                             0);
    const std::string errors = capture.StopAndGetErrors();
    EXPECT_EQ(conn, nullptr);
    EXPECT_EQ(errors.find("Failed to get connector"), std::string::npos)
        << "Expected no ALOGE on transient -ENOENT connector vanish, got: "
        << errors;
  }

  // 2. CreateInstance on a connector that existed during drmModeGetConnector()
  // but vanished (-ENODEV) before drmModeObjectGetProperties() in Init() (both
  // before required DPMS/CRTC_ID and during optional property discovery after
  // DPMS/CRTC_ID succeed) must return nullptr without emitting ALOGE.
  fake_device
      ->SetFakeRawConnectorForGetConnector(kMidInitVanishedMstConnId,
                                           /*encoder_id=*/kFirstEncoderId,
                                           DRM_MODE_CONNECTOR_DisplayPort);
  fake_device->SetGetObjectPropertiesFailureErrno(kMidInitVanishedMstConnId,
                                                  ENODEV);
  {
    ScopedErrorLogCapture capture;
    auto conn = DrmConnector::CreateInstance(*fake_device,
                                             kMidInitVanishedMstConnId, 1);
    const std::string errors = capture.StopAndGetErrors();
    EXPECT_EQ(conn, nullptr);
    EXPECT_TRUE(errors.empty())
        << "Expected zero ALOGE logs when connector vanishes mid-Init(), got: "
        << errors;
  }

  fake_device
      ->SetFakeRawConnectorForGetConnector(kPostCrtcIdVanishedMstConnId,
                                           /*encoder_id=*/kFirstEncoderId,
                                           DRM_MODE_CONNECTOR_DisplayPort);
  fake_device->RegisterFakeObjectProperty(kPostCrtcIdVanishedMstConnId, "DPMS",
                                          1);
  fake_device->RegisterFakeObjectProperty(kPostCrtcIdVanishedMstConnId,
                                          "CRTC_ID", 2);
  fake_device->SetGetObjectPropertiesFailureErrno(kPostCrtcIdVanishedMstConnId,
                                                  ENODEV);
  {
    ScopedErrorLogCapture capture;
    auto conn = DrmConnector::CreateInstance(*fake_device,
                                             kPostCrtcIdVanishedMstConnId, 1);
    const std::string errors = capture.StopAndGetErrors();
    EXPECT_EQ(conn, nullptr);
    EXPECT_TRUE(errors.empty())
        << "Expected zero ALOGE logs when connector vanishes during optional "
           "property discovery in Init(), got: "
        << errors;
  }

  // 3. UpdateModesAndProperties() on an existing MST connector that vanished
  // (-ENOENT) must mark the connector deleted and disconnected without ALOGE.
  auto* existing_mst = fake_device->AddPipelineResources(/*is_external=*/true);
  ASSERT_NE(existing_mst, nullptr);
  fake_device->SetGetConnectorFailureErrno(existing_mst->GetId(), ENOENT);
  {
    ScopedErrorLogCapture capture;
    const int ret = existing_mst->CallBaseUpdateModesAndProperties();
    const std::string errors = capture.StopAndGetErrors();
    EXPECT_EQ(ret, -ENODEV);
    EXPECT_TRUE(existing_mst->IsDeleted());
    EXPECT_EQ(errors.find("Failed to get connector"), std::string::npos)
        << "Expected no ALOGE on transient -ENOENT UpdateModesAndProperties(), "
           "got: "
        << errors;
  }

  // 4. Genuine hardware/driver I/O error (-EIO) MUST still emit ALOGE.
  fake_device->SetGetConnectorFailureErrno(kFaultyConnId, EIO);
  {
    ScopedErrorLogCapture capture;
    auto conn = DrmConnector::CreateInstance(*fake_device, kFaultyConnId, 2);
    const std::string errors = capture.StopAndGetErrors();
    EXPECT_EQ(conn, nullptr);
    EXPECT_NE(errors.find("Failed to get connector"), std::string::npos)
        << "Expected ALOGE on genuine -EIO connector failure, got: " << errors;
  }
}

TEST_F(DrmHwcTest,
       UEventListenerSkipsDuplicateSettleSweepWhenSettledButRetriesUnsettled) {
  const ScopedTestProperty device_prop("vendor.hwc.drm.device",
                                       "/nonexistent_drm_device");
  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  shared_dev->GetFakeAtomicCommitSink()->SetDriveStateManager(true);
  auto* internal_conn = shared_dev->AddPipelineResources(/*is_external=*/false);
  auto* ext_conn = shared_dev->AddPipelineResources(/*is_external=*/true);
  ASSERT_NE(internal_conn, nullptr);
  ASSERT_NE(ext_conn, nullptr);
  ext_conn->SetConnected(false);
  shared_dev->SetResourceConnectorIds(
      {internal_conn->GetId(), ext_conn->GetId()});

  std::atomic<size_t> refresh_calls{0};
  shared_dev->SetGetResourcesHook([&]() { ++refresh_calls; });

  auto* shared_dev_ptr = shared_dev.get();
  AddDeviceToResMan(std::move(shared_dev));
  InitResManForTest();

  int fds[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds), 0);
  auto uevent = UEvent::CreateInstanceWithFd(MakeUniqueFd(fds[0]));
  UniqueFd write_fd = MakeUniqueFd(fds[1]);
  ASSERT_NE(uevent, nullptr);
  StartUEventListenerForTest(std::move(uevent));

  constexpr int kPostSettleWaitMs = 280;
  const std::string hotplug_uevent = MakeUEventPacket(
      {"change@/devices/drm/card0", "DEVTYPE=drm_minor", "HOTPLUG=1"});

  // 1. Cleanly settled hotplug connect: ext_conn already has valid modes on the
  // initial uevent sweep, so no duplicate 200ms trailing settle sweep should
  // execute after the quiet timeout expires.
  {
    std::scoped_lock lock(hwc.GetResMan().GetMainLock());
    ext_conn->SetConnected(true);
    ext_conn->SetModes({CreateFakeDrmMode()});
  }
  refresh_calls.store(0);
  internal_conn->ResetUpdateModesCallCount();

  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kPostSettleWaitMs));

  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 1U);
  EXPECT_EQ(refresh_calls.load(), 1U)
      << "Cleanly settled hotplug ran a duplicate 200ms trailing settle sweep";
  EXPECT_EQ(internal_conn->GetUpdateModesCallCount(), 1U)
      << "Cleanly settled hotplug re-probed connector modes a second time";

  // Reset ext_conn to disconnected for Subtest 2.
  {
    std::scoped_lock lock(hwc.GetResMan().GetMainLock());
    ext_conn->SetConnected(false);
  }
  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kPostSettleWaitMs));
  ASSERT_EQ(GetAttachedPipelineCount(ext_conn), 0U);
  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();

  // 2. Unsettled initial HPD pulse (0 modes on first probe, valid modes ready
  // before the 200ms settle timeout with NO follow-up uevent): UEventListener
  // MUST execute the 200ms trailing settle retry and bind the display.
  constexpr int kInitialSweepAllowanceMs = 50;
  {
    std::scoped_lock lock(hwc.GetResMan().GetMainLock());
    ext_conn->SetConnected(true);
    ext_conn->SetModes({});
  }
  refresh_calls.store(0);
  ext_conn->ResetUpdateModesCallCount();

  ASSERT_EQ(write(*write_fd, hotplug_uevent.data(), hotplug_uevent.size()),
            static_cast<ssize_t>(hotplug_uevent.size()));
  std::this_thread::sleep_for(
      std::chrono::milliseconds(kInitialSweepAllowanceMs));
  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 0U);
  EXPECT_EQ(refresh_calls.load(), 1U);

  {
    std::scoped_lock lock(hwc.GetResMan().GetMainLock());
    ext_conn->SetModes({CreateFakeDrmMode()});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(kPostSettleWaitMs));

  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 1U)
      << "Unsettled 0-mode initial HPD pulse was not recovered by 200ms settle "
         "retry";
  EXPECT_EQ(refresh_calls.load(), 2U)
      << "Expected initial sweep + one 200ms settle retry for unsettled "
         "0-mode connector";

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  DeInitResManForTest();
}

TEST_F(DrmHwcTest,
       HotplugProbesConnectorsOutsideMainLockAndScopesByUeventConnectorId) {
  const ScopedTestProperty device_prop("vendor.hwc.drm.device",
                                       "/nonexistent_drm_device");
  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  shared_dev->GetFakeAtomicCommitSink()->SetDriveStateManager(true);
  auto* internal_conn = shared_dev->AddPipelineResources(/*is_external=*/false);
  auto* ext_conn = shared_dev->AddPipelineResources(/*is_external=*/true);
  ASSERT_NE(internal_conn, nullptr);
  ASSERT_NE(ext_conn, nullptr);
  ext_conn->SetConnected(false);
  shared_dev->SetResourceConnectorIds(
      {internal_conn->GetId(), ext_conn->GetId()});

  auto* shared_dev_ptr = shared_dev.get();
  AddDeviceToResMan(std::move(shared_dev));
  InitResManForTest();

  int fds[2] = {-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds), 0);
  auto uevent = UEvent::CreateInstanceWithFd(MakeUniqueFd(fds[0]));
  UniqueFd write_fd = MakeUniqueFd(fds[1]);
  ASSERT_NE(uevent, nullptr);
  StartUEventListenerForTest(std::move(uevent));

  auto is_main_lock_free_from_other_thread = [this]() {
    bool unlocked = false;
    std::thread probe_thread([this, &unlocked]() {
      if (hwc.GetResMan().GetMainLock().try_lock()) {
        hwc.GetResMan().GetMainLock().unlock();
        unlocked = true;
      }
    });
    probe_thread.join();
    return unlocked;
  };

  std::atomic<bool> resources_probed_unlocked{false};
  std::atomic<bool> modes_probed_unlocked{false};
  shared_dev_ptr->SetGetResourcesHook([&]() {
    resources_probed_unlocked.store(is_main_lock_free_from_other_thread());
  });
  ext_conn->SetUpdateModesHook([&]() {
    modes_probed_unlocked.store(is_main_lock_free_from_other_thread());
  });

  internal_conn->ResetUpdateModesCallCount();
  ext_conn->ResetUpdateModesCallCount();
  {
    std::scoped_lock lock(hwc.GetResMan().GetMainLock());
    ext_conn->SetConnected(true);
    ext_conn->SetModes({CreateFakeDrmMode()});
  }

  constexpr int kDeliveryWaitMs = 80;
  const std::string scoped_hotplug = MakeUEventPacket(
      {"change@/devices/drm/card0", "DEVTYPE=drm_minor", "HOTPLUG=1",
       "CONNECTOR=" + std::to_string(ext_conn->GetId())});

  ASSERT_EQ(write(*write_fd, scoped_hotplug.data(), scoped_hotplug.size()),
            static_cast<ssize_t>(scoped_hotplug.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(kDeliveryWaitMs));

  shared_dev_ptr->SetGetResourcesHook(nullptr);
  ext_conn->SetUpdateModesHook(nullptr);

  EXPECT_EQ(GetAttachedPipelineCount(ext_conn), 1U);
  EXPECT_TRUE(resources_probed_unlocked.load())
      << "drmModeGetResources() was called while holding GetMainLock()";
  EXPECT_TRUE(modes_probed_unlocked.load())
      << "UpdateModesAndProperties() was called while holding GetMainLock()";
  EXPECT_EQ(internal_conn->GetUpdateModesCallCount(), 0U)
      << "Settled internal connector was re-probed despite CONNECTOR="
      << ext_conn->GetId() << " scoping";
  EXPECT_GE(ext_conn->GetUpdateModesCallCount(), 1U);

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  DeInitResManForTest();
}

TEST_F(DrmHwcTest, OffLockHotplugProbeDoesNotOverwriteFresherMainLockReprobe) {
  auto shared_dev = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
  auto* shared_dev_ptr = shared_dev.get();

  constexpr uint32_t kExtEncoderId = 81;
  constexpr uint32_t kExtCrtcId = 82;
  constexpr uint32_t kExtConnectorId = 83;
  constexpr uint32_t kExtPlaneId = 183;

  auto enc = std::make_unique<FakeDrmEncoder>(kExtEncoderId, kExtCrtcId);
  enc->InitRawEncoder(kFakeAllCrtcsMask);
  shared_dev->AddEncoder(std::move(enc));
  shared_dev->AddCrtc(std::make_unique<FakeDrmCrtc>(kExtCrtcId));
  shared_dev->AddPlane(
      std::make_unique<FakeDrmPlane>(*shared_dev, DRM_PLANE_TYPE_PRIMARY,
                                     /*is_valid=*/true, kExtPlaneId));

  shared_dev->RegisterFakeObjectProperty(kExtConnectorId, "DPMS", 1);
  shared_dev->RegisterFakeObjectProperty(kExtConnectorId, "CRTC_ID", 2);
  shared_dev
      ->SetFakeRawConnectorForGetConnector(kExtConnectorId, kExtEncoderId,
                                           DRM_MODE_CONNECTOR_DisplayPort);
  auto real_conn = DrmConnector::CreateInstance(*shared_dev, kExtConnectorId,
                                                /*index=*/0);
  ASSERT_NE(real_conn, nullptr);
  auto* real_conn_ptr = real_conn.get();
  shared_dev->AddConnector(std::move(real_conn));
  shared_dev->SetResourceConnectorIds({kExtConnectorId});

  AddDeviceToResMan(std::move(shared_dev));

  // Initially Phase 2 off-lock probe sees a vanished/disconnected connector
  // (ENOENT), but while Phase 2 is in flight outside GetMainLock(), a
  // synchronous caller under GetMainLock() (e.g. SetPowerMode(kOn)) re-probes
  // the connector after it comes back online. Phase 3 must not overwrite that
  // fresher state with the stale Phase-2 ENOENT result.
  shared_dev_ptr->SetGetConnectorFailureErrno(kExtConnectorId, ENOENT);
  std::atomic<bool> hook_ran{false};
  shared_dev_ptr->SetGetConnectorHook([&](uint32_t id) {
    if (id != kExtConnectorId || hook_ran.exchange(true)) {
      return;
    }
    std::thread main_lock_prober([&]() {
      std::scoped_lock lock(hwc.GetResMan().GetMainLock());
      shared_dev_ptr->SetGetConnectorFailureErrno(kExtConnectorId,
                                                  std::nullopt);
      EXPECT_EQ(real_conn_ptr->UpdateModesAndProperties(), 0);
    });
    main_lock_prober.join();
  });

  UEventListener::HotplugEvent event{};
  event.connector_ids.insert(kExtConnectorId);
  InvokeHandleUEventHotplugForTest(event);

  shared_dev_ptr->SetGetConnectorHook(nullptr);
  EXPECT_TRUE(hook_ran.load());
  EXPECT_FALSE(real_conn_ptr->IsDeleted())
      << "Stale Phase-2 off-lock probe overwrote fresher synchronous "
         "UpdateModesAndProperties() state";
  EXPECT_TRUE(real_conn_ptr->IsConnected());

  shared_dev_ptr->GetFakeAtomicCommitSink()->ClearHistory();
  DeInitResManForTest();
}

TEST_F(DrmHwcTest,
       EarlyBootAnimationCommitsPureSeamlessPageflipWithoutPowerMode) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd, /*num_frames=*/2);

  const ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  const ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  const ScopedTestProperty hold_prop("vendor.hwc.drm.early_boot_hold_ms", "0");
  const ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  hwc.StartEarlyBootAnimation();
  hwc.WaitForCompletionAndStopEarlyBootAnimation();

  const auto& history = sink->GetCommitHistory();
  ASSERT_GE(history.size(), 2U);
  for (size_t i = 0; i < history.size(); ++i) {
    ASSERT_FALSE(history[i].empty());
    const auto& frame_args = history[i].front();
    EXPECT_TRUE(frame_args.seamless) << "Frame " << i << " was not seamless";
    EXPECT_FALSE(frame_args.display_mode.has_value())
        << "Frame " << i << " unexpectedly carried display_mode";
    EXPECT_FALSE(frame_args.power_mode.has_value())
        << "Frame " << i
        << " carried redundant power_mode, triggering spurious config-result "
           "telemetry and blocking nonblock flag evaluation";
  }

  sink->ClearHistory();
}

}  // namespace android::drm_hwcomposer
