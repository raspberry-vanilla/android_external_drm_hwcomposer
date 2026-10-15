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

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmHwc.h"
#include "drm/DrmMode.h"
#include "drm/DrmTestUtils.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

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

class TestDrmHwc : public DrmHwc {
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
  void SendHotplugEventToClient(DisplayHandle /*display_handle*/,
                                DisplayStatus /*display_status*/) override {
  }
  void SendHdcpLevelsChangedEventToClient(
      DisplayHandle /*display_handle*/,
      std::optional<HdcpContentType> /*current_hdcp_level*/) override {
  }
};

}  // namespace

struct HwcDisplayTest : public ::testing::Test {
  void SetUp() override {
    fake_device = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
    fake_device->GetFakeAtomicCommitSink()->SetDriveStateManager(true);
  }

  void TearDown() override {
    hwc.DeinitDisplays();
    fake_device.reset();
  }

  std::shared_ptr<DrmDisplayPipeline> CreatePipelineWithModes(
      const std::vector<DrmMode>& modes, bool is_external = false) const {
    fake_device->AddPipelineResourcesWithModes(modes, is_external);
    return fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
  }

  std::unique_ptr<FakeDrmDevice> fake_device;
  TestDrmHwc hwc;
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

}  // namespace android::drm_hwcomposer
