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

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <sys/types.h>
#include <unistd.h>

#include "compositor/DisplayInfo.h"
#include "compositor/HdcpController.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmMode.h"
#include "drm/DrmTestUtils.h"
#include "drm/ResourceManager.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "utils/TestUtils.h"
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

  std::shared_ptr<DrmDisplayPipeline> CreatePipelineWithModes(
      std::vector<DrmMode> modes, bool is_external = false) const {
    fake_device->AddPipelineResourcesWithModes(std::move(modes), is_external);
    return fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
  }

  ScopedTestProperty reset_bootanim_completed{"vendor.hwc.bootanim.completed",
                                              ""};
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
  auto secondary_pipe = CreatePipeline(/*is_external=*/true);
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

}  // namespace android::drm_hwcomposer
