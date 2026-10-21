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

#include "hwc/HwcDisplay.h"

#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <sys/types.h>
#include <unistd.h>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/HdcpController.h"
#include "compositor/IntelMappingValidator.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/CommitStatus.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmHwc.h"
#include "drm/DrmMode.h"
#include "drm/DrmTestUtils.h"
#include "hwc/HwcBufferCache.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/TestUtils.h"
#include "utils/fd.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {
constexpr int kMaxRetries = 20;
constexpr int kRetryDelayMs = 20;

namespace {

constexpr uint16_t kFhdWidth = 1920;
constexpr uint16_t kFhdHeight = 1080;
constexpr int32_t kFhdWidthSigned = 1920;
constexpr int32_t kFhdHeightSigned = 1080;
constexpr float kFhdWidthFloat = 1920.0F;
constexpr float kFhdHeightFloat = 1080.0F;
constexpr uint16_t kHdWidth = 1280;
constexpr uint16_t kHdHeight = 720;
constexpr uint32_t kRefresh60Hz = 60;
constexpr uint32_t kRefresh120Hz = 120;
constexpr uint32_t kTestFbId = 501;
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kTestAnimWidth = 4;
constexpr uint32_t kTestAnimHeight = 4;
constexpr uint8_t kTestPixelByte = 0x55;
constexpr int kVsyncObservationWindowMs = 50;

class TestDrmHwc : public DrmHwc {
 public:
  bool HasCallback() const override {
    return true;
  }
  void SendVsyncEventToClient(DisplayHandle /*display_handle*/,
                              int64_t /*timestamp*/,
                              uint32_t /*vsync_period*/) const override {
    ++vsync_count_;
  }
  void SendVsyncPeriodTimingChangedEventToClient(
      DisplayHandle /*display_handle*/, int64_t /*timestamp*/) const override {
  }
  void SendRefreshEventToClient(DisplayHandle /*display_handle*/) override {
    ++refresh_count_;
  }
  void SendHotplugEventToClient(DisplayHandle /*display_handle*/,
                                DisplayStatus /*display_status*/) override {
  }
  void SendHdcpLevelsChangedEventToClient(
      DisplayHandle /*display_handle*/,
      std::optional<HdcpContentType> current_hdcp_level) override {
    last_hdcp_level = current_hdcp_level;
    hdcp_callback_count++;
  }

  void ResetVsyncCount() const {
    vsync_count_.store(0);
  }

  int GetVsyncCount() const {
    return vsync_count_.load();
  }

  void ResetRefreshCount() {
    refresh_count_.store(0);
  }

  int GetRefreshCount() const {
    return refresh_count_.load();
  }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  std::optional<HdcpContentType> last_hdcp_level;
  int hdcp_callback_count = 0;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

 private:
  mutable std::atomic<int> vsync_count_{0};
  std::atomic<int> refresh_count_{0};
};

HwcLayer::LayerProperties CreateFullScreenFhdLayerProperties(
    std::shared_ptr<FakeFbIdHandle> fb) {
  BufferInfo bi{};
  bi.width = kFhdWidth;
  bi.height = kFhdHeight;
  bi.format = DRM_FORMAT_ABGR8888;
  bi.pitches[0] = kFhdWidth * kBytesPerPixel;
  bi.prime_fds[0] = 1;

  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{
      .bi = bi,
      .fb = std::move(fb),
      .fence = {},
  };
  props.display_frame = {
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned},
  };
  props.source_crop = {
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat},
  };
  props.blend_mode = BufferBlendMode::kNone;
  props.composition_type = CompositionType::kDevice;
  return props;
}

}  // namespace

struct HwcDisplayTest : public ::testing::Test {
  static void WriteValidRawAnimationFile(int fd) {
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
    hdr.fps = kRefresh60Hz;
    hdr.num_frames = 1;
    hdr.format = DRM_FORMAT_ARGB8888;
    hdr.hold_frame = 0;
    hdr.hold_duration_ms = 0;

    std::vector<uint8_t> pixels(static_cast<size_t>(hdr.width) * hdr.height *
                                    kBytesPerPixel,
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
    hwc.StopEarlyBootAnimation();
    test_display.reset();
    hwc.DeinitDisplays();
    fake_device.reset();
  }

  HwcDisplay* CreatePrimaryDisplay() {
    hwc.StopEarlyBootAnimation();
    test_display = std::make_unique<HwcDisplay>(kPrimaryDisplay,
                                                /*is_virtual=*/false, &hwc);
    return test_display.get();
  }

  std::shared_ptr<DrmDisplayPipeline> CreatePipelineWithModes(
      const std::vector<DrmMode>& modes, bool is_external = false) const {
    fake_device->AddPipelineResourcesWithModes(modes, is_external);
    return fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
  }

  ScopedTestProperty reset_bootanim_completed{"vendor.hwc.bootanim.completed",
                                              ""};
  std::unique_ptr<FakeDrmDevice> fake_device;
  TestDrmHwc hwc;
  std::unique_ptr<HwcDisplay> test_display;
};

TEST_F(HwcDisplayTest, HeadlessPrimaryDisplayInitializationAndPowerModes) {
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(nullptr);

  EXPECT_TRUE(display.IsInHeadlessMode());
  EXPECT_TRUE(display.GetDisplayEnabled());

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 1U);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, configs[0].id);

  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  EXPECT_EQ(display.SetPowerMode(PowerMode::kDoze),
            HwcDisplay::Error::kUnsupported);
}

TEST_F(HwcDisplayTest, PhysicalDisplaySetPipelineInitializesAndModesets) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  EXPECT_FALSE(display.IsInHeadlessMode());
  EXPECT_TRUE(display.GetDisplayEnabled());

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 2U);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetSize(),
            std::make_pair(uint32_t{kFhdWidth}, uint32_t{kFhdHeight}));
  EXPECT_GE(fake_device->GetFakeAtomicCommitSink()->GetExecuteCommitCount(),
            1U);
}

TEST_F(HwcDisplayTest, SetConfigSwitchesActiveModeAndCommitsToKms) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 2U);

  ConfigId target_config_id = 0;
  for (const auto& cfg : configs) {
    if (cfg.mode.GetRawMode().hdisplay == kHdWidth) {
      target_config_id = cfg.id;
      break;
    }
  }
  ASSERT_NE(target_config_id, 0U);

  fake_device->GetFakeAtomicCommitSink()->ClearHistory();
  EXPECT_EQ(display.SetConfig(target_config_id),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, target_config_id);
  EXPECT_EQ(display.GetSize(),
            std::make_pair(uint32_t{kHdWidth}, uint32_t{kHdHeight}));
  EXPECT_EQ(fake_device->GetFakeAtomicCommitSink()->GetExecuteCommitCount(),
            1U);
}

TEST_F(HwcDisplayTest, CreateValidateAndPresentDeviceLayerComposition) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  HwcLayer* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  props.composition_type = CompositionType::kDevice;
  props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  props.blend_mode = BufferBlendMode::kPreMult;
  props.z_order = 0;
  layer->SetLayerProperties(props);

  fake_device->GetFakeAtomicCommitSink()->ClearHistory();
  auto validate_res = display.ValidateStagedComposition();
  EXPECT_TRUE(validate_res.changed_layers.empty());

  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_EQ(fake_device->GetFakeAtomicCommitSink()->GetExecuteCommitCount(),
            1U);

  EXPECT_TRUE(display.DestroyLayer(kLayerId));
  EXPECT_EQ(display.get_layer(kLayerId), nullptr);
}

TEST_F(HwcDisplayTest,
       DeinitCoalescesPlaneDisableAndTeardownIntoSingleBlockingCommit) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  display.Deinit();

  ASSERT_EQ(sink->GetExecuteCommitCount(), 1U);
  const auto& history = sink->GetCommitHistory();
  ASSERT_EQ(history.size(), 1U);
  ASSERT_EQ(history[0].size(), 1U);
  const auto& deinit_args = history[0][0];
  EXPECT_TRUE(deinit_args.blocking);
  EXPECT_TRUE(deinit_args.teardown);
  EXPECT_EQ(deinit_args.power_mode, PowerMode::kOff);
  ASSERT_NE(deinit_args.composition, nullptr);
  EXPECT_TRUE(deinit_args.composition->plan.empty());
}

TEST_F(HwcDisplayTest,
       PresentZeroLayersWithValidClientTargetCommitsClientLayer) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  HwcLayer::LayerProperties client_props{};
  client_props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  client_props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  client_props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  client_props.blend_mode = BufferBlendMode::kPreMult;
  display.GetClientLayer().SetLayerProperties(client_props);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  // Present with 0 HwcLayers in display.layers() but a valid client_layer_.
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_EQ(sink->GetExecuteCommitCount(), 1U);
}

TEST_F(HwcDisplayTest, SetPowerModeOffStopsVsyncCallbacks) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  display.SetVsyncCallbacksEnabled(true);
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);

  hwc.ResetVsyncCount();
  std::this_thread::sleep_for(
      std::chrono::milliseconds(kVsyncObservationWindowMs));
  EXPECT_EQ(hwc.GetVsyncCount(), 0);
}

TEST_F(HwcDisplayTest, QueueSeamlessConfigUpdatesRequestedAndNextConfig) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh120Hz, 0, "1080p120")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 2U);
  ASSERT_EQ(configs[0].group_id, configs[1].group_id);

  const ConfigId initial_id = display.GetCurrentConfig()->id;
  const ConfigId next_id = (configs[0].id == initial_id) ? configs[1].id
                                                         : configs[0].id;

  QueuedConfigTiming timing{};
  EXPECT_EQ(display.QueueConfig(next_id, /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, next_id);
  ASSERT_NE(display.GetNextConfig(), nullptr);
  EXPECT_EQ(display.GetNextConfig()->id, next_id);
}

TEST_F(HwcDisplayTest, StartHdcpRepeatedNotifications) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  // Enable HDCP properties on the fake connector
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* fake_connector = static_cast<FakeDrmConnector*>(
      pipeline->connector->Get());
  fake_connector->SetContentProtectionPropertySupported(true);
  fake_connector->SetHdcpContentTypePropertySupported(true);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  EXPECT_TRUE(display.StartHdcp());

  // Simulate successful negotiation
  fake_connector->SetContentProtectionEnabled(true);

  // Setup a basic layer to trigger a frame update commit
  constexpr ILayerId kLayerId = 1;
  display.CreateLayer(kLayerId);
  HwcLayer* layer = display.get_layer(kLayerId);
  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;
  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{.bi = bo,
                                  .fb = std::make_shared<FakeFbIdHandle>(
                                      kTestFbId)};
  props.composition_type = CompositionType::kDevice;
  props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  layer->SetLayerProperties(props);

  fake_device->GetFakeAtomicCommitSink()->ClearHistory();
  display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  display.PresentStagedComposition(std::nullopt, present_fence, release_fences);

  const auto& history = fake_device->GetFakeAtomicCommitSink()
                            ->GetCommitHistory();
  ASSERT_FALSE(history.empty());
  ASSERT_FALSE(history.back().empty());
  auto args = history.back().front();

  EXPECT_EQ(args.content_protection, ContentProtection::kDesired);
  EXPECT_EQ(args.hdcp_content_type, HdcpContentType::kType1);

  ASSERT_NE(display.GetHdcpController(), nullptr);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  const_cast<HdcpController*>(display.GetHdcpController())->Requested();

  // Wait for the background thread to process the callback
  
  
  for (int i = 0; i < kMaxRetries; ++i) {
    if (hwc.last_hdcp_level == HdcpContentType::kType1)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
  }

  int count_before = hwc.hdcp_callback_count;
  EXPECT_TRUE(display.StartHdcp());
  EXPECT_EQ(hwc.hdcp_callback_count, count_before + 1);
  EXPECT_EQ(hwc.last_hdcp_level, HdcpContentType::kType1);
}

TEST_F(HwcDisplayTest, StopHdcpTeardown) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto* fake_connector = static_cast<FakeDrmConnector*>(
      pipeline->connector->Get());
  fake_connector->SetContentProtectionPropertySupported(true);
  fake_connector->SetHdcpContentTypePropertySupported(true);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  EXPECT_TRUE(display.StartHdcp());

  fake_connector->SetContentProtectionEnabled(true);

  constexpr ILayerId kLayerId = 1;
  display.CreateLayer(kLayerId);
  HwcLayer* layer = display.get_layer(kLayerId);
  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;
  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{.bi = bo,
                                  .fb = std::make_shared<FakeFbIdHandle>(
                                      kTestFbId)};
  props.composition_type = CompositionType::kDevice;
  props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  layer->SetLayerProperties(props);

  fake_device->GetFakeAtomicCommitSink()->ClearHistory();
  display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  display.PresentStagedComposition(std::nullopt, present_fence, release_fences);

  ASSERT_NE(display.GetHdcpController(), nullptr);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  const_cast<HdcpController*>(display.GetHdcpController())->Requested();
  
  
  for (int i = 0; i < kMaxRetries; ++i) {
    if (hwc.last_hdcp_level == HdcpContentType::kType1)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
  }

  EXPECT_TRUE(display.StopHdcp());
  
  
  for (int i = 0; i < kMaxRetries; ++i) {
    if (hwc.last_hdcp_level == std::nullopt)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
  }

  EXPECT_EQ(hwc.last_hdcp_level, std::nullopt);

  fake_device->GetFakeAtomicCommitSink()->ClearHistory();
  display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence_2;
  std::vector<HwcDisplay::ReleaseFence> release_fences_2;
  display.PresentStagedComposition(std::nullopt, present_fence_2,
                                   release_fences_2);

  const auto& history2 = fake_device->GetFakeAtomicCommitSink()
                             ->GetCommitHistory();
  ASSERT_FALSE(history2.empty());
  auto args2 = history2.back().front();
  EXPECT_NE(args2.content_protection, ContentProtection::kDesired);
}

TEST_F(HwcDisplayTest,
       PresentZeroLayersWithGenericLayerMapperPlannerCommitsClientLayer) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);
  pipeline->planner = std::make_unique<GenericLayerMapperCompositionPlanner>();

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  HwcLayer::LayerProperties client_props{};
  client_props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  client_props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  client_props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  client_props.blend_mode = BufferBlendMode::kPreMult;
  display.GetClientLayer().SetLayerProperties(client_props);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_EQ(sink->GetExecuteCommitCount(), 1U);
}

TEST_F(HwcDisplayTest,
       FullSuspendResumeLifecycleWithGenericLayerMapperPlanner) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);
  pipeline->planner = std::make_unique<GenericLayerMapperCompositionPlanner>(
      IntelValidateMapping);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  display.SetVsyncCallbacksEnabled(true);
  ASSERT_TRUE(display.GetDisplayEnabled());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId active_config_id = display.GetCurrentConfig()->id;

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  // Populate client_layer_ as SurfaceFlinger does during normal operation.
  HwcLayer::LayerProperties client_props{};
  client_props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  client_props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  client_props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  client_props.blend_mode = BufferBlendMode::kPreMult;
  display.GetClientLayer().SetLayerProperties(client_props);

  // Step 1: Present an active frame with a device layer before suspend.
  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  HwcLayer* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);
  HwcLayer::LayerProperties layer_props = client_props;
  layer_props.composition_type = CompositionType::kDevice;
  layer_props.z_order = 0;
  layer->SetLayerProperties(layer_props);

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));

  // Step 2: Pre-suspend screen-off blanking frame. SurfaceFlinger destroys all
  // HwcLayers while client_layer_ still holds its previous buffer, then runs
  // validate + present with 0 HwcLayers.
  ASSERT_TRUE(display.DestroyLayer(kLayerId));
  ASSERT_TRUE(display.layers().empty());

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));

  // Step 3: Power off the display for suspend.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  EXPECT_FALSE(display.GetDisplayEnabled());

  // Step 4: Simulate transient modeset failure on initial resume attempt (e.g.
  // MST hub link training not ready yet), verifying config state stays valid.
  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->SetNextExecuteStatus(CommitStatus::Failure(-ENOSPC));
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn),
            HwcDisplay::Error::kBadParameter);
  EXPECT_FALSE(display.GetDisplayEnabled());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, active_config_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, active_config_id);

  // Step 5: Retry SetPowerMode(kOn) once the link is ready; display re-enables.
  sink->SetNextExecuteStatus(CommitStatus::Success());
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  EXPECT_TRUE(display.GetDisplayEnabled());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, active_config_id);

  // Step 6: Post-resume frame presentation with recreated HwcLayer.
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);
  layer->SetLayerProperties(layer_props);

  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_EQ(sink->GetExecuteCommitCount(), 1U);
}

TEST_F(HwcDisplayTest,
       HeadlessToPhysicalTransitionReimportsCachedClientTargetSlot) {
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(nullptr);
  ASSERT_TRUE(display.IsInHeadlessMode());

  // Mirror ComposerClientUtils::ImportFb: returns nullptr while headless, and
  // delegates to GetPipe().importer once a physical pipeline is attached.
  HwcBufferCache client_cache([&display](BufferInfo& bo) {
    if (display.IsInHeadlessMode()) {
      return std::shared_ptr<DrmFbIdHandle>(nullptr);
    }
    return display.GetPipe().importer->GetOrCreateFbId(&bo);
  });

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  // Frame 1 while headless: slot 0 is populated with .fb = nullptr.
  client_cache.SetSlot(0, bo);
  EXPECT_EQ(client_cache.GetFb(0), nullptr);

  // Hotplug attaches a physical pipeline to kPrimaryDisplay.
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);
  display.SetPipeline(pipeline);
  ASSERT_FALSE(display.IsInHeadlessMode());

  // Frame 2 after transition reuses cached slot 0 on client_layer_.
  auto cached_bi = client_cache.GetBufferInfo(0);
  ASSERT_TRUE(cached_bi.has_value());
  if (!cached_bi.has_value()) {
    return;
  }
  auto cached_fb = client_cache.GetFb(0);
  EXPECT_NE(cached_fb, nullptr);

  HwcLayer::LayerProperties client_props{};
  client_props.buffer = HwcLayer::Buffer{
      .bi = *cached_bi,
      .fb = cached_fb,
      .fence = {},
  };
  client_props.blend_mode = BufferBlendMode::kPreMult;
  display.GetClientLayer().SetLayerProperties(client_props);

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  HwcLayer* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);
  HwcLayer::LayerProperties layer_props{};
  layer_props.composition_type = CompositionType::kClient;
  layer->SetLayerProperties(layer_props);

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
}

TEST_F(HwcDisplayTest, CreateModesetCommitOmitsModesetLayerWhenFbImportFails) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 2U);
  const ConfigId target_config_id = configs[1].id;

  // Dumb buffer allocation succeeds, but FB import fails (e.g. during MST
  // unplug or connector teardown). GetModesetLayerData must not produce a
  // LayerData with .fb == nullptr that poisons the modeset composition plan.
  fake_device->GetFakeFbImporter()->SetAllowImport(false);

  auto commit_args = display.CreateModesetCommit(target_config_id);
  ASSERT_TRUE(commit_args.has_value());
  if (!commit_args.has_value()) {
    return;
  }
  ASSERT_NE(commit_args->composition, nullptr);
  for (const auto& joining : commit_args->composition->plan) {
    EXPECT_NE(joining.layer.fb, nullptr);
  }
}

TEST_F(HwcDisplayTest, FailedModesetCommitDoesNotCorruptStagedConfigState) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 2U);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_config_id = display.GetCurrentConfig()->id;
  const ConfigId target_config_id = (configs[0].id == initial_config_id)
                                        ? configs[1].id
                                        : configs[0].id;

  // Simulate atomic commit failure (-EINVAL) during modeset (as in
  // ComposerClient::SetActiveConfigs -> CreateModesetCommit +
  // ExecuteAtomicCommit).
  fake_device->GetFakeAtomicCommitSink()->SetNextExecuteStatus(
      CommitStatus::Failure(-EINVAL));

  auto commit_args = display.CreateModesetCommit(target_config_id);
  ASSERT_TRUE(commit_args.has_value());
  if (!commit_args.has_value()) {
    return;
  }
  auto status_or_results = fake_device->GetAtomicCommitSink()
                               .ExecuteAtomicCommit(
                                   {{display.GetPipe()
                                         .atomic_state_manager.get(),
                                     *commit_args}});
  EXPECT_FALSE(status_or_results.IsSuccess());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, initial_config_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, initial_config_id);
}

TEST_F(HwcDisplayTest, FailedModesetResetsPreviouslyQueuedStagedConfig) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh120Hz, 0, "1080p120"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);

  // Seamless transitions are only valid across modes with identical resolution.
  fake_device->GetFakeAtomicCommitSink()->SetTestCommitHook(
      [](const auto& args) {
        for (const auto& [manager, commit_args] : args) {
          if (commit_args.seamless && commit_args.display_mode.has_value() &&
              commit_args.display_mode->GetRawMode().hdisplay != kFhdWidth) {
            return CommitStatus::Failure(-EINVAL);
          }
        }
        return CommitStatus::Success();
      });

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 3U);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_id = display.GetCurrentConfig()->id;
  const ConfigId seamless_id = configs[1].id;
  const ConfigId modeset_id = configs[2].id;

  QueuedConfigTiming timing{};
  ASSERT_EQ(display.QueueConfig(seamless_id, /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, seamless_id);

  // Even when a blocking SetConfig fails, any previously queued staged config
  // is now stale and must be reset.
  fake_device->GetFakeAtomicCommitSink()->SetNextExecuteStatus(
      CommitStatus::Failure(-EINVAL));
  EXPECT_EQ(display.SetConfig(modeset_id),
            HwcDisplay::ConfigError::kConfigFailed);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, initial_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, initial_id);
}

TEST_F(HwcDisplayTest, FailedInitialModesetInInitKeepsValidActiveConfigId) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  // Simulate initial modeset failure during Init() (e.g. ENOSPC on MST hub).
  fake_device->GetFakeAtomicCommitSink()->SetNextExecuteStatus(
      CommitStatus::Failure(-ENOSPC));

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto configs = display.GetDisplayConfigs();
  ASSERT_EQ(configs.size(), 1U);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, configs[0].id);
}

TEST_F(HwcDisplayTest, StandardBootAnimationStateLifecycle) {
  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "0");
  ScopedTestProperty exit_prop("service.bootanim.exit", "0");

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  EXPECT_EQ(display.GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);

  // Create two layers so one remains for validation after the bootanimation
  // layer is destroyed.
  constexpr ILayerId kBootAnimLayerId = 1;
  constexpr ILayerId kSystemUiLayerId = 2;
  ASSERT_TRUE(display.CreateLayer(kBootAnimLayerId));
  ASSERT_TRUE(display.CreateLayer(kSystemUiLayerId));

  // Destroying the first layer signals standard boot animation exit -> kCleanup
  EXPECT_TRUE(display.DestroyLayer(kBootAnimLayerId));
  EXPECT_EQ(display.GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);

  // Next ValidateStagedComposition transitions kCleanup -> kInactive.
  (void)display.ValidateStagedComposition();
  EXPECT_EQ(display.GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
}

TEST_F(HwcDisplayTest,
       StandardBootAnimationInactiveWhenCompletedOrExternalOrHeadless) {
  // 1. Standard boot animation already completed (e.g. UI restart or VTS test).
  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "0");
  {
    ScopedTestProperty exit_prop("service.bootanim.exit", "1");

    auto internal_pipe = CreatePipelineWithModes(
        {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                           DRM_MODE_TYPE_PREFERRED, "1080p60")},
        /*is_external=*/false);
    HwcDisplay internal_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
    internal_display.SetPipeline(internal_pipe);
    EXPECT_EQ(internal_display.GetBootAnimationState(),
              HwcDisplay::BootAnimationState::kInactive);
  }

  // 2. External display when boot animation is not yet completed stays
  // kInactive.
  ScopedTestProperty exit_prop("service.bootanim.exit", "0");
  auto external_pipe = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  HwcDisplay external_display(1, /*is_virtual=*/false, &hwc);
  external_display.SetPipeline(external_pipe);
  EXPECT_EQ(external_display.GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);

  // 3. Headless display stays kInactive.
  HwcDisplay headless_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  headless_display.SetPipeline(nullptr);
  EXPECT_EQ(headless_display.GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
}

TEST_F(HwcDisplayTest,
       EarlyBootAnimationInitSuccessSetsActiveAndIgnoresDestroyLayer) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd);

  // Set service.bootanim.exit=1 to verify early boot path ignores the standard
  // boot animation property.
  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);
  ScopedTestProperty exit_prop("service.bootanim.exit", "1");

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  HwcDisplay* display = CreatePrimaryDisplay();
  display->SetPipeline(pipeline);

  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);

  // Layer destruction must NOT transition early boot animation state.
  constexpr ILayerId kLayerId1 = 1;
  constexpr ILayerId kLayerId2 = 2;
  ASSERT_TRUE(display->CreateLayer(kLayerId1));
  ASSERT_TRUE(display->CreateLayer(kLayerId2));
  EXPECT_TRUE(display->DestroyLayer(kLayerId1));
  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);

  // Stopping early boot animation transitions kActive -> kCleanup.
  hwc.StopEarlyBootAnimation();
  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());

  // Next ValidateStagedComposition transitions kCleanup -> kInactive.
  (void)display->ValidateStagedComposition();
  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
}

TEST_F(HwcDisplayTest, EarlyBootAnimationInitFailureLeavesStateInactive) {
  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path",
                               "/tmp/nonexistent_drm_hwcomposer_bootanim.raw");

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  HwcDisplay* display = CreatePrimaryDisplay();
  display->SetPipeline(pipeline);

  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
}

TEST_F(HwcDisplayTest,
       EarlyBootAnimationAlreadyCompletedLeavesStateInactiveOnReinit) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd);

  // Start with early boot animation active on the primary display instance,
  // then re-initialize the same HwcDisplay instance after completion.
  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);
  ScopedTestProperty exit_prop("service.bootanim.exit", "0");

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  HwcDisplay* display = CreatePrimaryDisplay();
  display->SetPipeline(pipeline);
  ASSERT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kActive);

  // Stop early boot animation (state becomes kCleanup, completed=1), then
  // re-initialize the same HwcDisplay instance.
  hwc.StopEarlyBootAnimation();
  ASSERT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kCleanup);
  ASSERT_TRUE(Properties::EarlyBootAnimationCompleted());

  display->SetPipeline(pipeline);
  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);

  // Re-initializing from kActive to headless (nullptr) also resets to
  // kInactive.
  display->SetBootAnimationState(HwcDisplay::BootAnimationState::kActive);
  display->SetPipeline(nullptr);
  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
}

TEST_F(HwcDisplayTest,
       InitSetConfigFailureResetsBootAnimationStateAndStopsBootAnim) {
  TemporaryFile tf;
  WriteValidRawAnimationFile(tf.fd);

  ScopedTestProperty enable_prop("vendor.hwc.drm.bootanim.enable", "1");
  ScopedTestProperty completed_prop("vendor.hwc.bootanim.completed", "0");
  ScopedTestProperty path_prop("ro.vendor.hwc.bootanim.path", tf.path);

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);

  fake_device->GetFakeAtomicCommitSink()->SetNextExecuteStatus(
      CommitStatus::Failure(-EINVAL));

  HwcDisplay* display = CreatePrimaryDisplay();
  display->SetPipeline(pipeline);

  EXPECT_EQ(display->GetBootAnimationState(),
            HwcDisplay::BootAnimationState::kInactive);
  EXPECT_TRUE(Properties::EarlyBootAnimationCompleted());
}

TEST_F(HwcDisplayTest, InitPreservesConfigsWhenPreferredModesetFails) {
  constexpr uint16_t kUhdWidth = 3840;
  constexpr uint16_t kUhdHeight = 2160;
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kUhdWidth, kUhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "2160p60"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz, 0, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->SetNextExecuteStatus(CommitStatus::Failure(-ENOSPC));

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  EXPECT_FALSE(display.IsInHeadlessMode());
  EXPECT_EQ(display.GetDisplayConfigs().size(), 2U);
}

TEST_F(HwcDisplayTest,
       SetConfigActivatesDisplayAfterPreferredModesetFailsInInit) {
  constexpr uint16_t kUhdWidth = 3840;
  constexpr uint16_t kUhdHeight = 2160;
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kUhdWidth, kUhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "2160p60"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz, 0, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->SetNextExecuteStatus(CommitStatus::Failure(-ENOSPC));

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_FALSE(pipeline->atomic_state_manager->IsActive());

  ConfigId fallback_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.mode.GetRawMode().hdisplay == kFhdWidth) {
      fallback_config_id = cfg.id;
      break;
    }
  }
  ASSERT_NE(fallback_config_id, 0U);

  sink->SetNextExecuteStatus(CommitStatus::Success());
  EXPECT_EQ(display.SetConfig(fallback_config_id),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, fallback_config_id);
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());
}

TEST_F(HwcDisplayTest,
       FrameUpdateActivatesInactiveDisplayAfterPreferredModesetFailsInInit) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  // Simulate initial preferred-mode SetConfig failure during Init() (e.g.
  // -ENOSPC while another MST stream is temporarily occupying link bandwidth).
  sink->SetNextExecuteStatus(CommitStatus::Failure(-ENOSPC));

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  EXPECT_FALSE(display.IsInHeadlessMode());
  EXPECT_FALSE(display.GetDisplayEnabled());
  EXPECT_FALSE(pipeline->atomic_state_manager->IsActive());

  // Bandwidth becomes available (e.g. sibling MST display lowers resolution on
  // user login), and SurfaceFlinger validates + presents a frame at the
  // already-selected config without issuing a new SetConfig or SetPowerMode.
  sink->SetNextExecuteStatus(CommitStatus::Success());
  sink->ClearHistory();

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  HwcLayer* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  props.composition_type = CompositionType::kDevice;
  props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  props.blend_mode = BufferBlendMode::kPreMult;
  props.z_order = 0;
  layer->SetLayerProperties(props);

  auto validate_res = display.ValidateStagedComposition();
  EXPECT_TRUE(validate_res.changed_layers.empty());
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_TRUE(display.GetDisplayEnabled());
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());

  ASSERT_EQ(sink->GetCommitHistory().size(), 1U);
  ASSERT_EQ(sink->GetCommitHistory()[0].size(), 1U);
  const auto& first_frame_args = sink->GetCommitHistory()[0][0];
  EXPECT_TRUE(first_frame_args.display_mode.has_value());
  EXPECT_EQ(first_frame_args.power_mode, PowerMode::kOn);
  EXPECT_FALSE(first_frame_args.seamless);
  EXPECT_TRUE(first_frame_args.blocking);
}

TEST_F(HwcDisplayTest, FrameUpdateActivatesDisplayAfterSetPowerModeOnFails) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_TRUE(pipeline->atomic_state_manager->IsActive());

  EXPECT_EQ(display.SetPowerMode(HwcDisplay::PowerMode::kOff),
            HwcDisplay::Error::kNone);
  ASSERT_FALSE(pipeline->atomic_state_manager->IsActive());

  // Simulate SetPowerMode(kOn) failing (e.g. -ENOSPC during dock resume). It
  // must stage PowerMode::kOn so the next frame update retries activating the
  // display.
  sink->SetNextExecuteStatus(CommitStatus::Failure(-ENOSPC));
  EXPECT_EQ(display.SetPowerMode(HwcDisplay::PowerMode::kOn),
            HwcDisplay::Error::kBadParameter);
  ASSERT_FALSE(pipeline->atomic_state_manager->IsActive());

  sink->SetNextExecuteStatus(CommitStatus::Success());
  sink->ClearHistory();

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  HwcLayer* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  props.composition_type = CompositionType::kDevice;
  props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  props.blend_mode = BufferBlendMode::kPreMult;
  props.z_order = 0;
  layer->SetLayerProperties(props);

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_TRUE(display.GetDisplayEnabled());
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());

  ASSERT_EQ(sink->GetCommitHistory().size(), 1U);
  ASSERT_EQ(sink->GetCommitHistory()[0].size(), 1U);
  const auto& frame_args = sink->GetCommitHistory()[0][0];
  EXPECT_TRUE(frame_args.display_mode.has_value());
  EXPECT_EQ(frame_args.power_mode, PowerMode::kOn);
  EXPECT_FALSE(frame_args.seamless);
  EXPECT_TRUE(frame_args.blocking);
}

TEST_F(HwcDisplayTest, FrameUpdateDoesNotStageModesetWhenDisplayIsPoweredOff) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_TRUE(pipeline->atomic_state_manager->IsActive());

  EXPECT_EQ(display.SetPowerMode(HwcDisplay::PowerMode::kOff),
            HwcDisplay::Error::kNone);
  ASSERT_FALSE(pipeline->atomic_state_manager->IsActive());

  sink->ClearHistory();

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  HwcLayer* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);

  BufferInfo bo{};
  bo.width = kFhdWidth;
  bo.height = kFhdHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kFhdWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;

  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{
      .bi = bo,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  props.composition_type = CompositionType::kDevice;
  props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  props.source_crop = SrcRectInfo{
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat}};
  props.blend_mode = BufferBlendMode::kPreMult;
  props.z_order = 0;
  layer->SetLayerProperties(props);

  // Without a pending staged_power_mode_, CreateFrameUpdateCommit() must not
  // stage power_mode, display_mode, or a blocking modeset on an intentionally
  // powered-off display.
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  (void)display.PresentStagedComposition(std::nullopt, present_fence,
                                         release_fences);
  ASSERT_EQ(sink->GetCommitHistory().size(), 1U);
  ASSERT_EQ(sink->GetCommitHistory()[0].size(), 1U);
  const auto& frame_args = sink->GetCommitHistory()[0][0];
  EXPECT_FALSE(frame_args.display_mode.has_value());
  EXPECT_FALSE(frame_args.display_config_id.has_value());
  EXPECT_FALSE(frame_args.power_mode.has_value());
  EXPECT_FALSE(frame_args.blocking);
}

TEST_F(HwcDisplayTest, BlockingSetConfigRequestsClientRefresh) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->SetNextTestStatus(CommitStatus::Failure(-EINVAL));

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  sink->SetNextTestStatus(CommitStatus::Success());

  ConfigId hd_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.mode.GetRawMode().hdisplay == kHdWidth) {
      hd_config_id = cfg.id;
      break;
    }
  }
  ASSERT_NE(hd_config_id, 0U);

  hwc.ResetRefreshCount();
  EXPECT_EQ(display.SetConfig(hd_config_id), HwcDisplay::ConfigError::kNone);
  EXPECT_GT(hwc.GetRefreshCount(), 0);
}

TEST_F(HwcDisplayTest, BlockingSetConfigResetsLastPresentedComposition) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->SetNextTestStatus(CommitStatus::Failure(-EINVAL));

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  sink->SetNextTestStatus(CommitStatus::Success());

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  auto* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);

  BufferInfo bi{};
  bi.width = kFhdWidth;
  bi.height = kFhdHeight;
  bi.format = DRM_FORMAT_ABGR8888;
  bi.pitches[0] = kFhdWidth * kBytesPerPixel;
  bi.prime_fds[0] = 1;

  HwcLayer::LayerProperties props{};
  props.buffer = HwcLayer::Buffer{
      .bi = bi,
      .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
      .fence = {},
  };
  props.display_frame = {
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned},
  };
  props.source_crop = {
      .f_rect = FRect{0.0F, 0.0F, kFhdWidthFloat, kFhdHeightFloat},
  };
  props.blend_mode = BufferBlendMode::kNone;
  props.composition_type = CompositionType::kDevice;
  layer->SetLayerProperties(props);

  // Present a frame at 1080p60 so last_presented_composition_ is populated.
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));

  ConfigId hd_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.mode.GetRawMode().hdisplay == kHdWidth) {
      hd_config_id = cfg.id;
      break;
    }
  }
  ASSERT_NE(hd_config_id, 0U);

  // Blocking modeset to 720p60 must reset last_presented_composition_ so the
  // next ValidateStagedComposition() runs a test commit rather than
  // short-circuiting on the pre-modeset cache.
  EXPECT_EQ(display.SetConfig(hd_config_id), HwcDisplay::ConfigError::kNone);
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  EXPECT_GT(sink->GetTestCommitCount(), 0U);
}

TEST_F(HwcDisplayTest, SameResolutionModesetReusesActiveDeviceLayer) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh120Hz, 0, "1080p120")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_config_id = display.GetCurrentConfig()->id;
  ConfigId fhd_120_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.id != initial_config_id) {
      fhd_120_config_id = cfg.id;
      break;
    }
  }
  ASSERT_NE(fhd_120_config_id, 0U);

  constexpr ILayerId kLayerId = 1;
  constexpr uint32_t kDeviceLayerFbId = 777;
  const auto device_fb = std::make_shared<FakeFbIdHandle>(kDeviceLayerFbId);
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  auto* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);
  layer->SetLayerProperties(CreateFullScreenFhdLayerProperties(device_fb));

  // Present a full-screen DEVICE layer while client_layer_ has no buffer set.
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_FALSE(display.GetClientLayer().IsLayerUsableAsDevice());

  const size_t imports_before_modeset = fake_device->GetFakeFbImporter()
                                            ->GetImportCount();
  sink->ClearHistory();

  // Switching to 1080p120 on an external display performs a blocking modeset at
  // the same resolution and must reuse the active DEVICE layer rather than
  // allocating and importing a zero-filled dumb buffer.
  EXPECT_EQ(display.SetConfig(fhd_120_config_id),
            HwcDisplay::ConfigError::kNone);
  EXPECT_EQ(fake_device->GetFakeFbImporter()->GetImportCount(),
            imports_before_modeset);
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().back().empty());
  const auto& modeset_args = sink->GetCommitHistory().back().front();
  ASSERT_NE(modeset_args.composition, nullptr);
  ASSERT_EQ(modeset_args.composition->plan.size(), 1U);
  EXPECT_EQ(modeset_args.composition->plan[0].layer.fb, device_fb);
}

TEST_F(HwcDisplayTest,
       SameResolutionModesetPrefersActiveDeviceLayerOverStaleClientLayer) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh120Hz, 0, "1080p120")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_config_id = display.GetCurrentConfig()->id;
  ConfigId fhd_120_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.id != initial_config_id) {
      fhd_120_config_id = cfg.id;
      break;
    }
  }
  ASSERT_NE(fhd_120_config_id, 0U);

  constexpr ILayerId kLayerId = 1;
  constexpr uint32_t kDeviceLayerFbId = 777;
  constexpr uint32_t kStaleClientFbId = 666;
  const auto device_fb = std::make_shared<FakeFbIdHandle>(kDeviceLayerFbId);
  const auto stale_client_fb = std::make_shared<FakeFbIdHandle>(
      kStaleClientFbId);
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  auto* layer = display.get_layer(kLayerId);
  ASSERT_NE(layer, nullptr);
  layer->SetLayerProperties(CreateFullScreenFhdLayerProperties(device_fb));

  // Populate client_layer_ with a stale buffer from an earlier CLIENT frame,
  // then validate and present a pure DEVICE frame (!NeedsClientLayerUpdate()).
  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(stale_client_fb));

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  EXPECT_TRUE(display.GetClientLayer().IsLayerUsableAsDevice());
  EXPECT_FALSE(display.NeedsClientLayerUpdate());

  sink->ClearHistory();
  EXPECT_EQ(display.SetConfig(fhd_120_config_id),
            HwcDisplay::ConfigError::kNone);
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().back().empty());
  const auto& modeset_args = sink->GetCommitHistory().back().front();
  ASSERT_NE(modeset_args.composition, nullptr);
  ASSERT_EQ(modeset_args.composition->plan.size(), 1U);
  EXPECT_EQ(modeset_args.composition->plan[0].layer.fb, device_fb);
}

}  // namespace android::drm_hwcomposer
