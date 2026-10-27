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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <aidl/android/hardware/graphics/common/Hdr.h>  // IWYU pragma: keep
#include <android-base/file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <sys/types.h>
#include <unistd.h>

#include "backend/BackendDisplayCapabilities.h"
#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/HdcpController.h"
#include "compositor/IntelMappingValidator.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmHwc.h"
#include "drm/DrmMode.h"
#include "drm/DrmTestUtils.h"
#include "drm/ResourceManager.h"
#include "hwc/HwcBufferCache.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/ColorUtil.h"
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
constexpr size_t kCtmRScaleIdx = 0;
constexpr size_t kCtmGScaleIdx = 5;
constexpr size_t kCtmBScaleIdx = 10;
constexpr size_t kCtmBOffsetIdx = 14;
constexpr float kCtmRScale = 0.8F;
constexpr float kCtmGScale = 0.9F;
constexpr float kCtmBScale = 0.7F;
constexpr float kCtmBOffset = 0.25F;
constexpr float kFloatTolerance = 1e-5F;
constexpr uint32_t kColorPipelinePrimaryPlaneId = 980;
constexpr uint32_t kTestOverlayPlaneId = 990;
constexpr float kOpaqueAlpha = 1.0F;
constexpr uint32_t kCursorTestPrimaryPlaneId = 991;
constexpr uint32_t kCursorTestOverlayPlaneId = 992;
constexpr uint32_t kCursorTestCursorPlaneId = 993;
constexpr float kNightLightRScale = 0.85F;
constexpr float kNightLightGScale = 0.75F;
constexpr float kNightLightBScale = 0.55F;
constexpr float kSolidRed = 0.5F;
constexpr float kSolidGreen = 0.25F;
constexpr float kSolidBlue = 0.75F;
constexpr float kSolidAlpha = 1.0F;
constexpr float kHalfBrightness = 0.5F;
constexpr float kExpectedScaledHalfBrightness = 0.6F;
constexpr float kOutOfRangeBrightness = 1.5F;

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

void StageSingleDeviceLayer(HwcDisplay& display, ILayerId layer_id = 1) {
  ASSERT_TRUE(display.CreateLayer(layer_id));
  HwcLayer* layer = display.get_layer(layer_id);
  ASSERT_NE(layer, nullptr);
  HwcLayer::LayerProperties props = CreateFullScreenFhdLayerProperties(
      std::make_shared<FakeFbIdHandle>(kTestFbId));
  props.blend_mode = BufferBlendMode::kPreMult;
  props.z_order = 0;
  layer->SetLayerProperties(props);
}

class FakeDisplayCapabilities : public BackendDisplayCapabilities {
 public:
  explicit FakeDisplayCapabilities(
      std::optional<std::vector<ColorMode>> color_modes,
      std::optional<std::vector<Hdr>> hdr_types = std::nullopt,
      std::optional<bool> hw_ctm_override = std::nullopt)
      : color_modes_(std::move(color_modes)),
        hdr_types_(std::move(hdr_types)),
        hw_ctm_override_(hw_ctm_override) {
  }

  std::optional<std::vector<ColorMode>> GetColorModeOverrides() const override {
    return color_modes_;
  }

  std::optional<std::vector<Hdr>> GetHdrTypesOverride() const override {
    return hdr_types_;
  }

  std::optional<bool> GetHardwareColorTransformOverride() const override {
    return hw_ctm_override_;
  }

 private:
  std::optional<std::vector<ColorMode>> color_modes_;
  std::optional<std::vector<Hdr>> hdr_types_;
  std::optional<bool> hw_ctm_override_;
};

class SpyAtomicStateManager : public DrmAtomicStateManager {
 public:
  enum class Step {
    kWaitLastFrame,
    kGetAtomicModeReqForArgs,
  };

  explicit SpyAtomicStateManager(DrmDisplayPipeline* pipe)
      : DrmAtomicStateManager(pipe) {
  }

  void SetNextPresentFence(SharedFd fence) {
    next_present_fence_ = std::move(fence);
  }

  void SetTrackedFence(SharedFd fence) {
    tracked_fence_ = std::move(fence);
    fence_use_count_at_wait_last_frame_ = 0;
    fence_use_count_at_commit_ = 0;
    steps_.clear();
  }

  void WaitLastFrame() override {
    if (steps_.empty()) {
      steps_.push_back(Step::kWaitLastFrame);
      fence_use_count_at_wait_last_frame_ = tracked_fence_.use_count();
    }
    DrmAtomicStateManager::WaitLastFrame();
  }

  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs& args) override {
    steps_.push_back(Step::kGetAtomicModeReqForArgs);
    fence_use_count_at_commit_ = tracked_fence_.use_count();
    return DrmAtomicStateManager::GetAtomicModeReqForArgs(args);
  }

  AtomicCommitResult FinishPendingRequest(
      const AtomicCommitArgs& args,
      std::unique_ptr<AtomicRequest> request) override {
    auto res = DrmAtomicStateManager::FinishPendingRequest(args,
                                                           std::move(request));
    if (next_present_fence_) {
      res.present_fence = std::move(next_present_fence_);
    }
    return res;
  }

  const std::vector<Step>& GetSteps() const {
    return steps_;
  }

  int64_t GetFenceUseCountAtWaitLastFrame() const {
    return fence_use_count_at_wait_last_frame_;
  }

  int64_t GetFenceUseCountAtCommit() const {
    return fence_use_count_at_commit_;
  }

 private:
  SharedFd next_present_fence_;
  SharedFd tracked_fence_;
  int64_t fence_use_count_at_wait_last_frame_ = 0;
  int64_t fence_use_count_at_commit_ = 0;
  std::vector<Step> steps_;
};

#if HAS_LIBDISPLAY_INFO
constexpr size_t kEdidSize = 128;
constexpr size_t kEdidHeaderByteCount = 6;
constexpr size_t kEdidManufacturerIdOffset = 8;
constexpr size_t kEdidProductCodeOffset = 10;
constexpr size_t kEdidWeekOfManufactureOffset = 16;
constexpr size_t kEdidYearOfManufactureOffset = 17;
constexpr size_t kEdidVersionOffset = 18;
constexpr size_t kEdidRevisionOffset = 19;
constexpr size_t kEdidVideoInputDefOffset = 20;
constexpr size_t kEdidScreenSizeOffset = 0x15;
constexpr size_t kEdidScreenHeightCmOffset = 22;
constexpr size_t kEdidDisplayGammaOffset = 23;
constexpr size_t kEdidFeatureSupportOffset = 24;
constexpr size_t kEdidChromaticityOffset = 25;
constexpr size_t kEdidFirstDtdOffset = 0x36;
constexpr size_t kDtdWidthMmOffset = 12;
constexpr size_t kDtdHeightMmOffset = 13;
constexpr size_t kDtdHighMmOffset = 14;
constexpr size_t kDtdFlagsOffset = 17;
constexpr size_t kEdidChecksumOffset = 127;
constexpr size_t kEdidExtensionCountOffset = 126;
constexpr uint8_t kCtaExtensionTag = 0x02;
constexpr uint8_t kCtaExtensionRevision = 0x03;
constexpr size_t kCtaDtdOffsetByte = 2;
constexpr size_t kCtaFirstDtdOffset = 4;
constexpr uint8_t kDummyDescriptorTag = 0x10;
constexpr uint8_t kDigitalVideoInputDef = 0x80;
constexpr uint8_t kDefaultGammaByte = 120;
constexpr uint8_t kDefaultFeatureByte = 0x02;

std::vector<uint8_t> MakeEdidWithBounds(uint16_t horiz_mm, uint16_t vert_mm,
                                        uint8_t width_cm, uint8_t height_cm) {
  std::vector<uint8_t> edid(kEdidSize, 0);
  for (size_t i = 1; i <= kEdidHeaderByteCount; ++i) {
    edid[i] = 0xFF;
  }
  edid[kEdidManufacturerIdOffset] = 0x1E;
  edid[kEdidManufacturerIdOffset + 1] = 0x6D;
  edid[kEdidProductCodeOffset] = 0x01;
  edid[kEdidProductCodeOffset + 1] = 0x00;
  edid[kEdidWeekOfManufactureOffset] = 1;
  edid[kEdidYearOfManufactureOffset] = 34;
  edid[kEdidVersionOffset] = 1;
  edid[kEdidRevisionOffset] = 4;
  edid[kEdidVideoInputDefOffset] = kDigitalVideoInputDef;
  edid[kEdidScreenSizeOffset] = width_cm;
  edid[kEdidScreenHeightCmOffset] = height_cm;
  edid[kEdidDisplayGammaOffset] = kDefaultGammaByte;
  edid[kEdidFeatureSupportOffset] = kDefaultFeatureByte;
  const uint8_t kSrgbChromaticity[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C,
                                         0x99, 0x26, 0x0F, 0x50, 0x54};
  std::memcpy(&edid[kEdidChromaticityOffset], kSrgbChromaticity,
              sizeof(kSrgbChromaticity));

  // DTD 0 at byte 54: 1920x1080 @ 60Hz (148.50 MHz pixel clock = 14850).
  uint8_t* dtd = &edid[kEdidFirstDtdOffset];
  dtd[0] = 0x02;
  dtd[1] = 0x3A;
  dtd[2] = 0x80;
  dtd[3] = 0x18;
  dtd[4] = 0x71;
  dtd[5] = 0x38;
  dtd[6] = 0x2D;
  dtd[7] = 0x40;
  dtd[8] = 0x58;
  dtd[9] = 0x2C;
  dtd[10] = 0x45;
  dtd[11] = 0x00;
  dtd[kDtdWidthMmOffset] = static_cast<uint8_t>(horiz_mm & 0xFFU);
  dtd[kDtdHeightMmOffset] = static_cast<uint8_t>(vert_mm & 0xFFU);
  dtd[kDtdHighMmOffset] = static_cast<uint8_t>(
      (((horiz_mm >> 8U) & 0x0FU) << 4U) | ((vert_mm >> 8U) & 0x0FU));
  dtd[15] = 0x00;
  dtd[16] = 0x00;
  dtd[kDtdFlagsOffset] = 0x1E;

  // Descriptors 1-3: Dummy descriptors (0x00 0x00 0x00 0x10 0x00).
  for (size_t desc_offset : {72U, 90U, 108U}) {
    edid[desc_offset + 3] = kDummyDescriptorTag;
  }

  uint8_t sum = 0;
  for (size_t i = 0; i < kEdidChecksumOffset; ++i) {
    sum = static_cast<uint8_t>(sum + edid[i]);
  }
  edid[kEdidChecksumOffset] = static_cast<uint8_t>(0U - sum);
  return edid;
}
std::vector<uint8_t> MakeEdidWithCtaExtensionBounds(
    uint16_t base_horiz_mm, uint16_t base_vert_mm, uint8_t width_cm,
    uint8_t height_cm, uint16_t cta_horiz_mm, uint16_t cta_vert_mm) {
  std::vector<uint8_t> edid = MakeEdidWithBounds(base_horiz_mm, base_vert_mm,
                                                 width_cm, height_cm);
  // Mark 1 extension block in the base EDID and recompute base checksum.
  edid[kEdidExtensionCountOffset] = 1;
  uint8_t base_sum = 0;
  for (size_t i = 0; i < kEdidChecksumOffset; ++i) {
    base_sum = static_cast<uint8_t>(base_sum + edid[i]);
  }
  edid[kEdidChecksumOffset] = static_cast<uint8_t>(0U - base_sum);

  // Append a 128-byte CTA-861 (tag 0x02, revision 3) extension block whose DTD
  // starts at offset 4.
  std::vector<uint8_t> cta(kEdidSize, 0);
  cta[0] = kCtaExtensionTag;
  cta[1] = kCtaExtensionRevision;
  cta[kCtaDtdOffsetByte] = static_cast<uint8_t>(kCtaFirstDtdOffset);
  cta[3] = 0x00;

  uint8_t* dtd = &cta[kCtaFirstDtdOffset];
  dtd[0] = 0x02;
  dtd[1] = 0x3A;
  dtd[2] = 0x80;
  dtd[3] = 0x18;
  dtd[4] = 0x71;
  dtd[5] = 0x38;
  dtd[6] = 0x2D;
  dtd[7] = 0x40;
  dtd[8] = 0x58;
  dtd[9] = 0x2C;
  dtd[10] = 0x45;
  dtd[11] = 0x00;
  dtd[kDtdWidthMmOffset] = static_cast<uint8_t>(cta_horiz_mm & 0xFFU);
  dtd[kDtdHeightMmOffset] = static_cast<uint8_t>(cta_vert_mm & 0xFFU);
  dtd[kDtdHighMmOffset] = static_cast<uint8_t>(
      (((cta_horiz_mm >> 8U) & 0x0FU) << 4U) | ((cta_vert_mm >> 8U) & 0x0FU));
  dtd[15] = 0x00;
  dtd[16] = 0x00;
  dtd[kDtdFlagsOffset] = 0x1E;

  uint8_t cta_sum = 0;
  for (size_t i = 0; i < kEdidChecksumOffset; ++i) {
    cta_sum = static_cast<uint8_t>(cta_sum + cta[i]);
  }
  cta[kEdidChecksumOffset] = static_cast<uint8_t>(0U - cta_sum);

  edid.insert(edid.end(), cta.begin(), cta.end());
  return edid;
}
#endif

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

  void ReloadProperties() {
    hwc.GetResMan().ReloadProperties();
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

// Pins the HwcDisplay contract for a same-group (seamless) config change whose
// frame commit is rejected by the kernel:
//  - QueueConfig() stages the config and enables VSync timestamp tracking, and
//    CreateFrameUpdateCommit() folds the staged config into the next frame
//    commit as a seamless display_mode.
//  - If the kernel rejects that commit, the config was never applied. The HAL
//    must then (a) keep presenting at the still-active config instead of
//    re-sending the rejected mode with every later frame, (b) stop reporting
//    the rejected config from GetLastRequestedConfig(), which backs hwc3
//    getActiveConfig(), (c) disable VSync timestamp tracking once the staged
//    mode is cleared, and (d) request a client refresh so SurfaceFlinger
//    re-presents if it would otherwise go idle.
TEST_F(HwcDisplayTest, RejectedStagedModeCommitDoesNotPoisonLaterFrames) {
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
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_id = display.GetCurrentConfig()->id;
  const ConfigId next_id = (configs[0].id == initial_id) ? configs[1].id
                                                         : configs[0].id;
  ASSERT_NE(display.GetConfig(next_id), nullptr);

  StageSingleDeviceLayer(display);

  // Reject every commit that carries a display mode (the seamless fold-in of
  // the staged config as well as any full-modeset retry of it), like a kernel
  // that no longer accepts this mode change. Commits without a mode succeed.
  using ExecuteResults = std::vector<
      std::pair<AtomicStateManager*, AtomicCommitResult>>;
  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->SetExecuteCommitHook(
      [](const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&
             args) -> CommitStatusOr<ExecuteResults> {
        ExecuteResults results;
        for (const auto& [manager, commit_args] : args) {
          if (commit_args.display_mode.has_value()) {
            return CommitStatusOr<ExecuteResults>(
                CommitStatus::Failure(-EINVAL));
          }
          results.emplace_back(manager, AtomicCommitResult{});
        }
        return CommitStatusOr<ExecuteResults>(results);
      });

  // desired_time 0 puts staged_mode_change_time_ in the past, so the staged
  // config is folded into the very next frame commit.
  QueuedConfigTiming timing{};
  ASSERT_EQ(display.QueueConfig(next_id, /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, next_id);
  EXPECT_TRUE(display.IsVsyncTimestampTrackingEnabled());

  // Frame 1 carries the staged mode and is rejected.
  hwc.ResetRefreshCount();
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_FALSE(display.PresentStagedComposition(std::nullopt, present_fence,
                                                release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().front().empty());
  const auto rejected_args = sink->GetCommitHistory().front().front();
  ASSERT_TRUE(rejected_args.display_mode.has_value());
  if (!rejected_args.display_mode.has_value()) {
    return;
  }
  EXPECT_TRUE(rejected_args.seamless);
  EXPECT_TRUE(*rejected_args.display_mode ==
              display.GetConfig(next_id)->mode.GetRawMode());
  EXPECT_EQ(hwc.GetRefreshCount(), 1);

  // (b) & (c) The rejected config was never applied: the active config is
  // unchanged, the HAL must no longer report the rejected config as
  // requested/active, and VSync timestamp tracking must be disabled.
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, initial_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, initial_id);
  EXPECT_FALSE(display.IsVsyncTimestampTrackingEnabled());

  // (a) The following frames must be presented at the still-active config
  // without re-sending the rejected mode.
  for (int frame = 2; frame <= 3; ++frame) {
    SCOPED_TRACE("frame " + std::to_string(frame));
    sink->ClearHistory();
    (void)display.ValidateStagedComposition();
    display.AcceptValidatedComposition();
    EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                                 release_fences));
    ASSERT_FALSE(sink->GetCommitHistory().empty());
    for (const auto& commit : sink->GetCommitHistory()) {
      ASSERT_FALSE(commit.empty());
      EXPECT_FALSE(commit.front().display_mode.has_value());
    }
  }
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, initial_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, initial_id);
}

TEST_F(HwcDisplayTest,
       FailedFrameCommitBeforeStagedConfigDuePreservesStagedConfig) {
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
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_id = display.GetCurrentConfig()->id;
  const ConfigId next_id = (configs[0].id == initial_id) ? configs[1].id
                                                         : configs[0].id;

  StageSingleDeviceLayer(display);

  // Queue a seamless config far in the future so it is not due on the next
  // frame commit.
  constexpr int64_t kFutureOffsetNs = 10'000'000'000LL;
  QueuedConfigTiming timing{};
  ASSERT_EQ(display.QueueConfig(next_id,
                                ResourceManager::GetTimeMonotonicNs() +
                                    kFutureOffsetNs,
                                &timing),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, next_id);
  EXPECT_TRUE(display.IsVsyncTimestampTrackingEnabled());

  // Make a plane-only frame commit fail before the staged config is due.
  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  sink->SetNextExecuteStatus(CommitStatus::Failure(-EINVAL));
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_FALSE(display.PresentStagedComposition(std::nullopt, present_fence,
                                                release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().front().empty());
  EXPECT_FALSE(sink->GetCommitHistory().front().front().display_mode);

  // Because the failed commit did not carry the not-yet-due staged mode,
  // the staged config and VSync timestamp tracking must remain intact.
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, initial_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, next_id);
  EXPECT_TRUE(display.IsVsyncTimestampTrackingEnabled());
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
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh120Hz, 0, "1080p120"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  const ConfigId initial_config_id = display.GetCurrentConfig()->id;
  ConfigId fhd_120_config_id = 0;
  ConfigId hd_60_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.mode.GetRawMode().hdisplay == kFhdWidth &&
        cfg.id != initial_config_id) {
      fhd_120_config_id = cfg.id;
    } else if (cfg.mode.GetRawMode().hdisplay == kHdWidth) {
      hd_60_config_id = cfg.id;
    }
  }
  ASSERT_NE(fhd_120_config_id, 0U);
  ASSERT_NE(hd_60_config_id, 0U);

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

  // QueueConfig across different resolution groups (1080p -> 720p) is rejected
  // with kSeamlessNotAllowed.
  QueuedConfigTiming queued_timing{};
  EXPECT_EQ(display.QueueConfig(hd_60_config_id, 0, &queued_timing),
            HwcDisplay::ConfigError::kSeamlessNotAllowed);

  // Cross-resolution modeset (1080p120 -> 720p60) cannot reuse the 1080p layer
  // and allocates + imports a new 1280x720 dumb buffer.
  sink->ClearHistory();
  EXPECT_EQ(display.SetConfig(hd_60_config_id), HwcDisplay::ConfigError::kNone);
  EXPECT_EQ(fake_device->GetFakeFbImporter()->GetImportCount(),
            imports_before_modeset + 1U);
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().back().empty());
  const auto& hd_modeset_args = sink->GetCommitHistory().back().front();
  ASSERT_NE(hd_modeset_args.composition, nullptr);
  ASSERT_EQ(hd_modeset_args.composition->plan.size(), 1U);
  ASSERT_TRUE(hd_modeset_args.composition->plan[0].layer.bi.has_value());
  const BufferInfo hd_modeset_bi = hd_modeset_args.composition->plan[0]
                                       .layer.bi.value_or(BufferInfo{});
  EXPECT_EQ(hd_modeset_bi.width, kHdWidth);
  EXPECT_EQ(hd_modeset_bi.height, kHdHeight);
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

TEST_F(HwcDisplayTest,
       GetColorModesReturnsExpectedModesAcrossCapabilitiesAndForcedColorMode) {
  // 1. Headless display returns {kNative}.
  HwcDisplay headless_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  headless_display.SetPipeline(nullptr);
  EXPECT_EQ(headless_display.GetColorModes(),
            std::vector<ColorMode>{ColorMode::kNative});

  // 2. Backend capability override returns overridden ColorModes and standard
  // + vendor boosted render intents.
  auto cap_pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(cap_pipeline, nullptr);
  const std::vector<ColorMode> k_override_modes = {ColorMode::kNative,
                                                   ColorMode::kSrgb,
                                                   ColorMode::kDisplayP3};
  cap_pipeline->capabilities = std::make_unique<FakeDisplayCapabilities>(
      k_override_modes);
  HwcDisplay cap_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  cap_display.SetPipeline(cap_pipeline);
  EXPECT_EQ(cap_display.GetColorModes(), k_override_modes);

  const auto intents = cap_display.GetRenderIntents(ColorMode::kSrgb);
  constexpr size_t kExpectedIntentsCount = 3U;
  EXPECT_EQ(intents.size(), kExpectedIntentsCount);
  EXPECT_NE(std::find(intents.begin(), intents.end(),
                      kVendorBoostedRenderIntent),
            intents.end());

  // 3. Internal display with forced_color_mode_ (DisplayP3 = 9) and color
  // pipeline enabled returns {kNative, kSrgb, kDisplayP3}.
  ScopedTestProperty use_cp_prop("vendor.hwc.drm.enable_color_pipeline", "1");
  ScopedTestProperty force_cm_prop("vendor.hwc.drm.force_color_mode", "9");
  ReloadProperties();

  auto forced_pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(forced_pipeline, nullptr);
  HwcDisplay forced_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  forced_display.SetPipeline(forced_pipeline);

  const std::vector<ColorMode> expected_forced_modes = {ColorMode::kNative,
                                                        ColorMode::kSrgb,
                                                        ColorMode::kDisplayP3};
  EXPECT_EQ(forced_display.GetColorModes(), expected_forced_modes);
}

TEST_F(
    HwcDisplayTest,
    SetColorModeAndColorTransformMatrixLinearizeWithColorPipelineAndMultiplyBoostedIntent) {
  ScopedTestProperty use_cp_prop("vendor.hwc.drm.enable_color_pipeline", "1");
  ScopedTestProperty force_cm_prop("vendor.hwc.drm.force_color_mode", "-1");
  ReloadProperties();

  auto primary = std::make_unique<FakeDrmPlane>(*fake_device,
                                                DRM_PLANE_TYPE_PRIMARY,
                                                /*is_valid=*/true,
                                                kColorPipelinePrimaryPlaneId);
  FakeDrmPlane* primary_plane = primary.get();
  primary_plane->SetHasColorPipeline(true);
  fake_device->AddPlane(std::move(primary));

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);
  pipeline->capabilities = std::make_unique<
      FakeDisplayCapabilities>(std::nullopt, std::nullopt,
                               /*hw_ctm_override=*/true);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  EXPECT_TRUE(display.UseColorPipeline());

  // Non-identity scaling matrix that ToLinearCtm transforms for kDisplayP3.
  HalColorTransformMatrix client_ctm = kIdentityMatrix;
  client_ctm[kCtmRScaleIdx] = kCtmRScale;
  client_ctm[kCtmGScaleIdx] = kCtmGScale;
  client_ctm[kCtmBScaleIdx] = kCtmBScale;

  display.SetColorMode(ColorMode::kDisplayP3, kVendorBoostedRenderIntent);
  display.SetColorTransformMatrix(client_ctm);

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  display.get_layer(kLayerId)->SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));

  ASSERT_FALSE(sink->GetCommitHistory().empty());
  const auto& commit_args = sink->GetCommitHistory().back().front();
  EXPECT_EQ(commit_args.color_mode, ColorMode::kDisplayP3);
  ASSERT_NE(commit_args.color_matrix, nullptr);

  auto expected_linear_ctm = std::make_shared<HalColorTransformMatrix>(
      ColorUtil::ToLinearCtm(client_ctm, ColorMode::kDisplayP3));
  auto expected_combined = ColorUtil::Multiply(GetBoostedCTMPtr(),
                                               expected_linear_ctm);
  for (size_t i = 0; i < kIdentityMatrix.size(); ++i) {
    EXPECT_NEAR((*commit_args.color_matrix)[i], (*expected_combined)[i],
                kFloatTolerance);
  }
}

TEST_F(HwcDisplayTest,
       CtmWithOffsetFallsBackToGpuAndPreservesRenderIntentMatrixInDrm) {
  ScopedTestProperty ctm_prop("vendor.hwc.drm.ctm", "DRM_OR_GPU");
  ScopedTestProperty offset_prop("vendor.hwc.drm.force_ctm_handles_offset",
                                 "0");
  ReloadProperties();

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_FALSE(display.HasHardwareColorTransform());

  // Set a CTM with an offset exclusively in the blue translation channel
  // (index 14) and zero Z-scale (index 10) so checking index 10 instead of
  // index 14 fails to detect the offset.
  HalColorTransformMatrix blue_offset_ctm = kIdentityMatrix;
  blue_offset_ctm[kCtmBScaleIdx] = 0.0F;
  blue_offset_ctm[kCtmBOffsetIdx] = kCtmBOffset;
  display.SetColorMode(ColorMode::kSrgb, kVendorBoostedRenderIntent);
  display.SetColorTransformMatrix(blue_offset_ctm);
  EXPECT_TRUE(display.CtmByGpu());

  // Populate client_layer_ and a single DEVICE layer; because CtmByGpu() is
  // true and client_ctm_has_offset_ is true without hardware CTM_OFFSET,
  // CreateFrameUpdateCommit must pass render_intent_matrix_ (boosted CTM)
  // rather than the client offset matrix to KMS.
  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));
  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  display.get_layer(kLayerId)->SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId + 1)));

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  auto validate_res = display.ValidateStagedComposition();
  ASSERT_EQ(validate_res.changed_layers.size(), 1U);
  EXPECT_EQ(validate_res.changed_layers[0].second, CompositionType::kClient);
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  const auto& commit_args = sink->GetCommitHistory().back().front();
  EXPECT_EQ(commit_args.color_matrix, GetBoostedCTMPtr());

  // Under DRM_OR_IGNORE, CtmByGpu() is false, so CreateFrameUpdateCommit relies
  // directly on client_ctm_has_offset_ (ColorUtil::HasOffset) to strip the
  // offset matrix and commit render_intent_matrix_ (GetBoostedCTMPtr).
  ScopedTestProperty ignore_prop("vendor.hwc.drm.ctm", "DRM_OR_IGNORE");
  ReloadProperties();
  EXPECT_FALSE(display.CtmByGpu());
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  EXPECT_EQ(sink->GetCommitHistory().back().front().color_matrix,
            GetBoostedCTMPtr());
}

TEST_F(
    HwcDisplayTest,
    ValidateStagedCompositionMarksOccludedCachedLayerAndTransitionsToClientOnGpuCtm) {
  ScopedTestProperty ctm_prop("vendor.hwc.drm.ctm", "DRM_OR_GPU");
  ScopedTestProperty offset_prop("vendor.hwc.drm.force_ctm_handles_offset",
                                 "0");
  ReloadProperties();

  fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  fake_device->AddPlane(
      std::make_unique<FakeDrmPlane>(*fake_device, DRM_PLANE_TYPE_OVERLAY,
                                     /*is_valid=*/true, kTestOverlayPlaneId));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(
          *fake_device->GetConnectors().back());
  ASSERT_NE(pipeline, nullptr);
  pipeline->planner = std::make_unique<GenericLayerMapperCompositionPlanner>();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));

  // Layer 1: cached layer with alpha == 0.0F (occluded by cached carrier).
  // Layer 2: cached carrier layer with alpha == 1.0F.
  constexpr ILayerId kOccludedLayerId = 1;
  constexpr ILayerId kCarrierLayerId = 2;
  ASSERT_TRUE(display.CreateLayer(kOccludedLayerId));
  ASSERT_TRUE(display.CreateLayer(kCarrierLayerId));

  auto cached_occluded_props = CreateFullScreenFhdLayerProperties(
      std::make_shared<FakeFbIdHandle>(kTestFbId + 1));
  cached_occluded_props.alpha = 0.0F;
  cached_occluded_props.z_order = 0;
  display.get_layer(kOccludedLayerId)
      ->SetLayerProperties(cached_occluded_props);

  auto cached_carrier_props = CreateFullScreenFhdLayerProperties(
      std::make_shared<FakeFbIdHandle>(kTestFbId + 2));
  cached_carrier_props.alpha = kOpaqueAlpha;
  cached_carrier_props.z_order = 1;
  display.get_layer(kCarrierLayerId)->SetLayerProperties(cached_carrier_props);

  // Frame 1: LayerCachingMapper marks layer 1 as kDeviceOccluded, and
  // CreateLayerToPlaneJoiningPlan drops the occluded layer so only the carrier
  // layer is committed to KMS.
  auto validate_frame1 = display.ValidateStagedComposition();
  ASSERT_EQ(validate_frame1.changed_layers.size(), 1U);
  EXPECT_EQ(validate_frame1.changed_layers[0].first, kOccludedLayerId);
  EXPECT_EQ(validate_frame1.changed_layers[0].second,
            CompositionType::kDeviceOccluded);
  EXPECT_EQ(display.get_layer(kOccludedLayerId)->GetValidatedType(),
            CompositionType::kDeviceOccluded);
  display.AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));

  // Frame 2: Set a CTM with an offset so CtmByGpu() becomes true and forces all
  // layers to kClient. Because layer 1 was previously kDeviceOccluded, it must
  // NOT be omitted from changed_layers when transitioning to real kClient.
  HalColorTransformMatrix offset_ctm = kIdentityMatrix;
  offset_ctm[kCtmBOffsetIdx] = kCtmBOffset;
  display.SetColorTransformMatrix(offset_ctm);
  ASSERT_TRUE(display.CtmByGpu());

  auto validate_frame2 = display.ValidateStagedComposition();
  EXPECT_EQ(validate_frame2.changed_layers.size(), 2U);
  EXPECT_EQ(display.get_layer(kOccludedLayerId)->GetValidatedType(),
            CompositionType::kClient);
}

TEST_F(
    HwcDisplayTest,
    CursorPlaneNeedsColorPipelineWithholdsCursorPlaneWhenCtmOrGamutRequiresColorOps) {
  ScopedTestProperty use_cp_prop("vendor.hwc.drm.enable_color_pipeline", "1");
  ScopedTestProperty force_cm_prop("vendor.hwc.drm.force_color_mode", "-1");
  ReloadProperties();

  auto primary = std::make_unique<FakeDrmPlane>(*fake_device,
                                                DRM_PLANE_TYPE_PRIMARY,
                                                /*is_valid=*/true,
                                                kCursorTestPrimaryPlaneId);
  FakeDrmPlane* primary_plane = primary.get();
  primary_plane->SetHasColorPipeline(true);
  fake_device->AddPlane(std::move(primary));

  fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  fake_device->AddPlane(
      std::make_unique<FakeDrmPlane>(*fake_device, DRM_PLANE_TYPE_OVERLAY,
                                     /*is_valid=*/true,
                                     kCursorTestOverlayPlaneId));
  fake_device->AddPlane(
      std::make_unique<FakeDrmPlane>(*fake_device, DRM_PLANE_TYPE_CURSOR,
                                     /*is_valid=*/true,
                                     kCursorTestCursorPlaneId));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(
          *fake_device->GetConnectors().back());
  ASSERT_NE(pipeline, nullptr);
  pipeline->capabilities = std::make_unique<
      FakeDisplayCapabilities>(std::nullopt, std::nullopt,
                               /*hw_ctm_override=*/true);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_TRUE(display.UseColorPipeline());
  ASSERT_NE(display.GetCursorPlane(), nullptr);

  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));

  constexpr ILayerId kBgLayerId = 1;
  constexpr ILayerId kCursorLayerId = 2;
  ASSERT_TRUE(display.CreateLayer(kBgLayerId));
  ASSERT_TRUE(display.CreateLayer(kCursorLayerId));

  auto bg_props = CreateFullScreenFhdLayerProperties(
      std::make_shared<FakeFbIdHandle>(kTestFbId + 1));
  bg_props.colorspace = HwcColorspace::kBt709;
  bg_props.z_order = 0;
  display.get_layer(kBgLayerId)->SetLayerProperties(bg_props);

  auto cursor_props = CreateFullScreenFhdLayerProperties(
      std::make_shared<FakeFbIdHandle>(kTestFbId + 2));
  cursor_props.composition_type = CompositionType::kCursor;
  cursor_props.colorspace = HwcColorspace::kBt709;
  cursor_props.z_order = 1;
  display.get_layer(kCursorLayerId)->SetLayerProperties(cursor_props);
  HwcLayer* cursor_layer = display.get_layer(kCursorLayerId);
  ASSERT_NE(cursor_layer, nullptr);

  // (a) Identity CTM + matching colorspace (kBt709) -> cursor plane does NOT
  // need color pipeline and validates as CompositionType::kCursor.
  display.SetColorMode(ColorMode::kSrgb, static_cast<ui::RenderIntent>(0));
  display.SetColorTransformMatrix(kIdentityMatrix);
  EXPECT_FALSE(display.CursorPlaneNeedsColorPipeline(*cursor_layer));
  auto validate_identity = display.ValidateStagedComposition();
  EXPECT_TRUE(validate_identity.changed_layers.empty());
  EXPECT_EQ(cursor_layer->GetValidatedType(), CompositionType::kCursor);
  display.AcceptValidatedComposition();

  // (b) Non-identity Night Light CTM -> CursorPlaneNeedsColorPipeline returns
  // true and ValidateStagedComposition withholds the cursor plane, falling back
  // the cursor layer to CompositionType::kDevice on the available overlay
  // plane.
  HalColorTransformMatrix night_light_ctm = kIdentityMatrix;
  night_light_ctm[kCtmRScaleIdx] = kNightLightRScale;
  night_light_ctm[kCtmGScaleIdx] = kNightLightGScale;
  night_light_ctm[kCtmBScaleIdx] = kNightLightBScale;
  display.SetColorTransformMatrix(night_light_ctm);
  EXPECT_TRUE(display.CursorPlaneNeedsColorPipeline(*cursor_layer));
  auto validate_night_light = display.ValidateStagedComposition();
  EXPECT_NE(cursor_layer->GetValidatedType(), CompositionType::kCursor);
  EXPECT_EQ(cursor_layer->GetValidatedType(), CompositionType::kDevice);

  // (c) Identity CTM, but cursor layer colorspace (kDciP3) differs from
  // display colorspace (kBt709) -> CursorPlaneNeedsColorPipeline returns true
  // due to gamut adjustment matrix.
  display.SetColorTransformMatrix(kIdentityMatrix);
  HwcLayer::LayerProperties p3_cursor_props{};
  p3_cursor_props.colorspace = HwcColorspace::kDciP3;
  cursor_layer->SetLayerProperties(p3_cursor_props);
  EXPECT_TRUE(display.CursorPlaneNeedsColorPipeline(*cursor_layer));

  // (d) When UseColorPipeline() is false, CursorPlaneNeedsColorPipeline returns
  // false.
  ScopedTestProperty disable_cp_prop("vendor.hwc.drm.enable_color_pipeline",
                                     "0");
  ReloadProperties();
  primary_plane->SetHasColorPipeline(false);
  HwcDisplay no_cp_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  no_cp_display.SetPipeline(pipeline);
  EXPECT_FALSE(no_cp_display.UseColorPipeline());
  EXPECT_FALSE(no_cp_display.CursorPlaneNeedsColorPipeline(*cursor_layer));
}

TEST_F(HwcDisplayTest, CreateLayerToPlaneJoiningPlanSkipsSolidColorLayers) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));

  // Create a kSolidColor layer (no buffer/fb) alongside a kDevice layer.
  constexpr ILayerId kSolidColorLayerId = 1;
  constexpr ILayerId kDeviceLayerId = 2;
  ASSERT_TRUE(display.CreateLayer(kSolidColorLayerId));
  ASSERT_TRUE(display.CreateLayer(kDeviceLayerId));

  HwcLayer::LayerProperties solid_props{};
  solid_props.composition_type = CompositionType::kSolidColor;
  solid_props.solid_color = SolidColor{kSolidRed, kSolidGreen, kSolidBlue,
                                       kSolidAlpha};
  solid_props.display_frame = DstRectInfo{
      .i_rect = IRect{0, 0, kFhdWidthSigned, kFhdHeightSigned}};
  solid_props.z_order = 0;
  display.get_layer(kSolidColorLayerId)->SetLayerProperties(solid_props);

  auto device_props = CreateFullScreenFhdLayerProperties(
      std::make_shared<FakeFbIdHandle>(kTestFbId + 1));
  device_props.z_order = 1;
  display.get_layer(kDeviceLayerId)->SetLayerProperties(device_props);

  // 1. Direct present without validation populates composition_types with
  // CompositionType::kSolidColor; CreateLayerToPlaneJoiningPlan must skip the
  // bufferless kSolidColor layer and commit only the valid kDevice layer.
  display.get_layer(kSolidColorLayerId)
      ->SetValidatedType(CompositionType::kSolidColor);
  display.get_layer(kDeviceLayerId)->SetValidatedType(CompositionType::kDevice);
  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  const auto& commit_args = sink->GetCommitHistory().back().front();
  ASSERT_NE(commit_args.composition, nullptr);
  EXPECT_EQ(commit_args.composition->plan.size(), 1U);

  // 2. ValidateStagedComposition marks the kSolidColor layer as kClient.
  auto validate_res = display.ValidateStagedComposition();
  ASSERT_FALSE(validate_res.changed_layers.empty());
  EXPECT_EQ(validate_res.changed_layers.front().first, kSolidColorLayerId);
  EXPECT_EQ(validate_res.changed_layers.front().second,
            CompositionType::kClient);
}

TEST_F(HwcDisplayTest,
       PresentStagedCompositionCallsWaitLastFrameBeforeWaitForPresentTime) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  auto spy_manager = std::make_unique<SpyAtomicStateManager>(pipeline.get());
  SpyAtomicStateManager* spy_ptr = spy_manager.get();
  pipeline->atomic_state_manager = std::move(spy_manager);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  display.get_layer(kLayerId)->SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));

  // Frame 1: Present with a valid out-fence so PresentStagedComposition stores
  // a SharedFd copy in vsync_worker_->last_present_fence_.
  SharedFd tracked_out_fence = std::make_shared<int>(-1);
  spy_ptr->SetNextPresentFence(tracked_out_fence);

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd frame1_present_fence;
  std::vector<HwcDisplay::ReleaseFence> frame1_release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt,
                                               frame1_present_fence,
                                               frame1_release_fences));
  frame1_present_fence.reset();
  frame1_release_fences.clear();

  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  // Track tracked_out_fence in SpyAtomicStateManager: its use_count is 3
  // (tracked_out_fence + spy_ptr->tracked_fence_ +
  // vsync_worker_->last_present_fence_) until WaitForPresentTime() calls
  // vsync_worker_->GetNextVsyncTimestamp() ->
  // UpdateLastVsyncTimeWithPresentTime() which resets
  // vsync_worker_->last_present_fence_ and drops use_count to 2.
  constexpr int64_t kFenceUseCountBeforeWaitForPresentTime = 3;
  constexpr int64_t kFenceUseCountAfterWaitForPresentTime = 2;
  spy_ptr->SetTrackedFence(tracked_out_fence);
  ASSERT_EQ(tracked_out_fence.use_count(),
            kFenceUseCountBeforeWaitForPresentTime);

  SharedFd frame2_present_fence;
  std::vector<HwcDisplay::ReleaseFence> frame2_release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(/*desired_present_time=*/0,
                                               frame2_present_fence,
                                               frame2_release_fences));

  const std::vector<SpyAtomicStateManager::Step> expected_steps = {
      SpyAtomicStateManager::Step::kWaitLastFrame,
      SpyAtomicStateManager::Step::kGetAtomicModeReqForArgs,
  };
  EXPECT_EQ(spy_ptr->GetSteps(), expected_steps);
  EXPECT_EQ(spy_ptr->GetFenceUseCountAtWaitLastFrame(),
            kFenceUseCountBeforeWaitForPresentTime);
  EXPECT_EQ(spy_ptr->GetFenceUseCountAtCommit(),
            kFenceUseCountAfterWaitForPresentTime);
}

TEST_F(HwcDisplayTest,
       SetBrightnessScalesInternalDisplayAndZeroesNegativeAndExternal) {
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.2");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "1");

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  constexpr ILayerId kLayerId = 1;
  ASSERT_TRUE(display.CreateLayer(kLayerId));
  display.get_layer(kLayerId)->SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));

  auto* sink = fake_device->GetFakeAtomicCommitSink();

  // 1. Positive brightness [0, 1] on internal display linearly remaps into
  // [min_brightness, 1.0] (0.2 + 0.5 * 0.8 = 0.6).
  EXPECT_TRUE(display.SetBrightness(kHalfBrightness));
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_TRUE(sink->GetCommitHistory().back().front().brightness.has_value());
  EXPECT_NEAR(sink->GetCommitHistory().back().front().brightness.value_or(
                  -1.0F),
              kExpectedScaledHalfBrightness, kFloatTolerance);

  // 2. Out-of-range brightness values (> 1.0 or negative values other than
  // -1.0) are rejected and leave the previously committed brightness unchanged.
  EXPECT_FALSE(display.SetBrightness(kOutOfRangeBrightness));
  EXPECT_FALSE(display.SetBrightness(-kHalfBrightness));
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_TRUE(sink->GetCommitHistory().back().front().brightness.has_value());
  EXPECT_NEAR(sink->GetCommitHistory().back().front().brightness.value_or(
                  -1.0F),
              kExpectedScaledHalfBrightness, kFloatTolerance);

  // 3. -1.0F (turning backlight off per HAL API) sets brightness_ to 0.0F so
  // downstream color/gamma pipelines scale to 0.
  EXPECT_TRUE(display.SetBrightness(-1.0F));
  sink->ClearHistory();
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_TRUE(sink->GetCommitHistory().back().front().brightness.has_value());
  EXPECT_FLOAT_EQ(sink->GetCommitHistory().back().front().brightness.value_or(
                      -1.0F),
                  0.0F);

  // 4. External display zeroes brightness_ even for positive brightness values.
  auto ext_pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(ext_pipeline, nullptr);
  HwcDisplay ext_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ext_display.SetPipeline(ext_pipeline);
  ASSERT_TRUE(ext_display.CreateLayer(kLayerId));
  ext_display.get_layer(kLayerId)->SetLayerProperties(
      CreateFullScreenFhdLayerProperties(
          std::make_shared<FakeFbIdHandle>(kTestFbId)));
  EXPECT_TRUE(ext_display.SetBrightness(kHalfBrightness));
  sink->ClearHistory();
  (void)ext_display.ValidateStagedComposition();
  ext_display.AcceptValidatedComposition();
  EXPECT_TRUE(ext_display.PresentStagedComposition(std::nullopt, present_fence,
                                                   release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_TRUE(sink->GetCommitHistory().back().front().brightness.has_value());
  EXPECT_FLOAT_EQ(sink->GetCommitHistory().back().front().brightness.value_or(
                      -1.0F),
                  0.0F);
}

TEST_F(
    HwcDisplayTest,
    SetPowerModeOnWhenAlreadyActiveIsNoOpAndAfterOffExecutesSingleModesetCommit) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_TRUE(pipeline->atomic_state_manager->IsActive());

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  const size_t imports_before = fake_device->GetFakeFbImporter()
                                    ->GetImportCount();

  // 1. Calling SetPowerMode(kOn) when already active returns early without
  // executing a modeset or empty atomic commit.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_EQ(fake_device->GetFakeFbImporter()->GetImportCount(), imports_before);

  // 2. Power off the display, then re-enable via SetPowerMode(kOn).
  // SetPowerMode(kOn) executes SetConfig() once and returns early without
  // issuing a redundant second empty atomic commit.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  ASSERT_FALSE(pipeline->atomic_state_manager->IsActive());

  sink->ClearHistory();
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());
  EXPECT_EQ(sink->GetExecuteCommitCount(), 1U);
}

TEST_F(HwcDisplayTest,
       RefreshConfigsFromConnectorDefersModesetUntilClientRequestsSetConfig) {
  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh120Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p120"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz, 0, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  ASSERT_TRUE(pipeline->atomic_state_manager->IsActive());
  ASSERT_EQ(display.GetDisplayConfigs().size(), 3U);

  // Populate a 1080p client target buffer so we also verify that the
  // subsequent SetConfig() does not reuse the stale 1080p buffer when
  // modesetting the pruned 720p preferred config.
  constexpr uint32_t kStaleClientFbId = 666;
  const auto client_fb = std::make_shared<FakeFbIdHandle>(kStaleClientFbId);
  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(client_fb));

  // Step 1: First DP link training failure prunes 1080p120, leaving
  // {1080p60 (preferred), 720p60}.
  fake_conn->SetModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60"),
       CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz, 0, "720p60")});
  fake_conn->SetLinkStatusPropertyValue(DRM_MODE_LINK_STATUS_BAD);

  sink->ClearHistory();
  EXPECT_TRUE(display.OnLinkTrainingFailed());
  EXPECT_EQ(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetDisplayConfigs().size(), 2U);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  const ConfigId first_fallback_id = display.GetLastRequestedConfig()->id;
  EXPECT_EQ(display.GetLastRequestedConfig()->mode.GetRawMode().hdisplay,
            kFhdWidth);

  // A duplicate link-status event with identical modes must not churn config
  // IDs while link_recovery_modeset_required_ is already true.
  EXPECT_TRUE(display.OnLinkTrainingFailed());
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, first_fallback_id);

  // Step 2: A second link failure steps down the link rate further and prunes
  // 1080p60 while link_recovery_modeset_required_ is still true, leaving only
  // {720p60 (preferred)}. OnLinkTrainingFailed() must detect that configs_
  // contains a mode no longer in connector->GetModes() and refresh configs_.
  fake_conn->SetModes({CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz,
                                         DRM_MODE_TYPE_PREFERRED, "720p60")});
  EXPECT_TRUE(display.OnLinkTrainingFailed());
  EXPECT_EQ(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetDisplayConfigs().size(), 1U);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  const ConfigId fallback_config_id = display.GetLastRequestedConfig()->id;
  EXPECT_EQ(display.GetLastRequestedConfig()->mode.GetRawMode().hdisplay,
            kHdWidth);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);

  // While link_recovery_modeset_required_ is set, QueueConfig() must return
  // kSeamlessNotAllowed so ComposerClient::setActiveConfigWithConstraints()
  // falls back to SetConfig() (ALLOW_MODESET) even though getActiveConfig()
  // already returns fallback_config_id.
  QueuedConfigTiming timing{};
  EXPECT_EQ(display.QueueConfig(fallback_config_id, /*desired_time=*/0,
                                &timing),
            HwcDisplay::ConfigError::kSeamlessNotAllowed);

  EXPECT_EQ(display.SetConfig(fallback_config_id),
            HwcDisplay::ConfigError::kNone);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, fallback_config_id);
  ASSERT_EQ(sink->GetExecuteCommitCount(), 1U);
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().back().empty());
  const auto& retrain_args = sink->GetCommitHistory().back().front();
  ASSERT_TRUE(retrain_args.display_mode.has_value());
  if (retrain_args.display_mode.has_value()) {
    EXPECT_EQ(retrain_args.display_mode->GetRawMode().hdisplay, kHdWidth);
    EXPECT_EQ(retrain_args.display_mode->GetRawMode().vdisplay, kHdHeight);
  }
  EXPECT_FALSE(retrain_args.seamless);
  EXPECT_EQ(retrain_args.power_mode, PowerMode::kOn);
  ASSERT_NE(retrain_args.composition, nullptr);
  ASSERT_EQ(retrain_args.composition->plan.size(), 1U);
  EXPECT_NE(retrain_args.composition->plan[0].layer.fb, client_fb);

  // If another link failure sets link_recovery_modeset_required_ = true and the
  // display is then unbound to headless mode before SetConfig(), Deinit() /
  // headless SetConfig() must clear link_recovery_modeset_required_.
  fake_conn->SetModes({CreateFakeDrmMode(kHdWidth, kHdHeight, kRefresh60Hz,
                                         DRM_MODE_TYPE_PREFERRED, "720p60")});
  EXPECT_TRUE(display.OnLinkTrainingFailed());
  EXPECT_EQ(display.QueueConfig(display.GetLastRequestedConfig()->id,
                                /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kSeamlessNotAllowed);
  display.SetPipeline(nullptr);
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.QueueConfig(display.GetCurrentConfig()->id,
                                /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kNone);
}

TEST_F(
    HwcDisplayTest,
    InternalDisplayInitWithNearOneHzPreferredModeSecondPreservesValidConfig) {
  constexpr uint32_t kClock5994HzKhz = 119880;
  constexpr uint32_t kClock6000HzKhz = 120000;
  drmModeModeInfo raw_5994{};
  raw_5994.hdisplay = kFhdWidth;
  raw_5994.vdisplay = kFhdHeight;
  raw_5994.htotal = kFakeModeHTotal;
  raw_5994.vtotal = kFakeModeVTotal;
  raw_5994.vrefresh = kRefresh60Hz;
  raw_5994.clock = kClock5994HzKhz;  // 119880 * 1000 / (2000 * 1000) = 59.94 Hz
  raw_5994.type = 0;
  std::strncpy(raw_5994.name, "1080p59_94", DRM_DISPLAY_MODE_LEN - 1);

  drmModeModeInfo raw_6000{};
  raw_6000.hdisplay = kFhdWidth;
  raw_6000.vdisplay = kFhdHeight;
  raw_6000.htotal = kFakeModeHTotal;
  raw_6000.vtotal = kFakeModeVTotal;
  raw_6000.vrefresh = kRefresh60Hz;
  raw_6000.clock = kClock6000HzKhz;  // 120000 * 1000 / (2000 * 1000) = 60.00 Hz
  raw_6000.type = DRM_MODE_TYPE_PREFERRED;
  std::strncpy(raw_6000.name, "1080p60", DRM_DISPLAY_MODE_LEN - 1);

  auto pipeline = CreatePipelineWithModes({DrmMode(&raw_5994),
                                           DrmMode(&raw_6000)},
                                          /*is_external=*/false);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  ASSERT_NE(display.GetCurrentConfig(), nullptr)
      << "HwcDisplay::GetCurrentConfig() returned nullptr after SanitizeGroups "
         "erased the preferred/active config ID during internal display Init()";
  EXPECT_STREQ(display.GetCurrentConfig()->mode.GetRawMode().name, "1080p60");
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
}

TEST_F(HwcDisplayTest,
       SetPowerModeOffPassesEmptyCompositionPlanToDisableActivePlanes) {
  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  ASSERT_TRUE(pipeline->atomic_state_manager->IsActive());

  // Verify that DrmAtomicStateManager disables active planes on teardown even
  // when args.composition is null, so HwcDisplay::SetPowerMode(PowerMode::kOff)
  // does not need to pass a dummy LayerToPlaneJoiningPlan (which would trigger
  // PresentDisplay() after power-off on SdmAtomicStateManager).
  const uint32_t primary_plane_id = pipeline->primary_plane->Get()->GetId();
  AtomicCommitArgs req_args{};
  req_args.blocking = true;
  req_args.power_mode = PowerMode::kOff;
  req_args.teardown = true;
  auto req = pipeline->atomic_state_manager->GetAtomicModeReqForArgs(req_args);
  ASSERT_NE(req, nullptr);
  void* raw_req = req.get();
  auto* drm_req = static_cast<DrmAtomicStateManager::DrmAtomicRequest*>(
      raw_req);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(),
                                      primary_plane_id, kFakePlaneCrtcPropId),
            0ULL);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(),
                                      primary_plane_id, kFakePlaneFbPropId),
            0ULL);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  ASSERT_EQ(sink->GetCommitHistory().size(), 1U);
  ASSERT_EQ(sink->GetCommitHistory()[0].size(), 1U);
  const auto& off_args = sink->GetCommitHistory()[0][0];
  EXPECT_EQ(off_args.power_mode, PowerMode::kOff);
  EXPECT_TRUE(off_args.teardown);
  EXPECT_EQ(off_args.composition, nullptr)
      << "HwcDisplay::SetPowerMode(PowerMode::kOff) must leave composition "
         "null and rely on teardown=true so SDM does not call PresentDisplay() "
         "after powering off";
}

TEST_F(HwcDisplayTest,
       SetPowerModeOnFailsWhenExternalConnectorDisconnectedDuringSuspend) {
  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);
  display.SetVsyncCallbacksEnabled(true);
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);

  // Simulate the external monitor being unplugged while the system was
  // suspended (PowerMode::kOff) before any resume uevent has been processed:
  // the cached connection status is still true until UpdateModesAndProperties()
  // probes the connector during SetPowerMode(PowerMode::kOn).
  ASSERT_TRUE(fake_conn->IsConnected());
  fake_conn->SetPendingConnected(false);
  const size_t calls_before = fake_conn->GetUpdateModesCallCount();

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  hwc.ResetVsyncCount();

  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn),
            HwcDisplay::Error::kBadParameter)
      << "HwcDisplay::SetPowerMode(PowerMode::kOn) succeeded on a connector "
         "that was disconnected during suspend";
  EXPECT_EQ(fake_conn->GetUpdateModesCallCount(), calls_before + 1U);
  EXPECT_TRUE(display.HasStagedPowerModeOn());
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  std::this_thread::sleep_for(
      std::chrono::milliseconds(kVsyncObservationWindowMs));
  EXPECT_EQ(hwc.GetVsyncCount(), 0)
      << "Failed SetPowerMode(PowerMode::kOn) on a disconnected external "
         "connector must not re-enable VSyncWorker callbacks";
}

TEST_F(HwcDisplayTest,
       ExternalDisplayInitDefersKmsModesetAndSkipsRedundantProbesAndOffCommit) {
  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  fake_conn->ResetUpdateModesCallCount();

  // External display (handle = 1 -> DisplayType::kExternal) defers its KMS
  // modeset in Init() and skips redundant UpdateModesAndProperties() when
  // ResourceManager has already populated connector modes.
  HwcDisplay display(DisplayHandle{1}, /*is_virtual=*/false, &hwc);
  EXPECT_TRUE(display.SetPipeline(pipeline));
  EXPECT_EQ(display.GetDisplayType(), HwcDisplay::DisplayType::kExternal);
  EXPECT_EQ(fake_conn->GetUpdateModesCallCount(), 0U);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_FALSE(pipeline->atomic_state_manager->IsActive());
  EXPECT_FALSE(display.HasStagedPowerModeOn())
      << "Deferred external Init() must not report HasStagedPowerModeOn(), or "
         "a follow-up NotifyDisplayConnected() will modeset on UEventThread";
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);

  // SurfaceFlinger + DisplayPowerController's initial SetPowerMode(kOff) on the
  // newly registered display is a no-op because the CRTC is not yet active.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_EQ(fake_conn->GetUpdateModesCallCount(), 0U);

  // DisplayPowerController's subsequent SetPowerMode(kOn) performs the single
  // blocking KMS modeset without a redundant connector probe.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  EXPECT_EQ(fake_conn->GetUpdateModesCallCount(), 0U);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 1U);
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());

  // After a real power-off teardown (suspend), the next SetPowerMode(kOn) must
  // re-probe UpdateModesAndProperties() once.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 2U);
  EXPECT_FALSE(pipeline->atomic_state_manager->IsActive());

  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  EXPECT_EQ(fake_conn->GetUpdateModesCallCount(), 1U);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 3U);
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());
}

TEST_F(HwcDisplayTest,
       ExternalDisplayQueueConfigWhileInactiveStagesClientModeForPowerOn) {
  constexpr uint16_t kUhdWidth = 3840;
  constexpr uint16_t kUhdHeight = 2160;
  constexpr uint32_t kRefresh30Hz = 30;
  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kUhdWidth, kUhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "2160p60"),
       CreateFakeDrmMode(kUhdWidth, kUhdHeight, kRefresh30Hz, 0, "2160p30"),
       CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz, 0, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();

  HwcDisplay display(DisplayHandle{1}, /*is_virtual=*/false, &hwc);
  EXPECT_TRUE(display.SetPipeline(pipeline));
  ASSERT_FALSE(pipeline->atomic_state_manager->IsActive());

  ConfigId uhd_60_config_id = 0;
  ConfigId uhd_30_config_id = 0;
  ConfigId fhd_config_id = 0;
  for (const auto& cfg : display.GetDisplayConfigs()) {
    if (cfg.mode.GetRawMode().hdisplay == kUhdWidth &&
        cfg.mode.GetRawMode().vrefresh == kRefresh60Hz) {
      uhd_60_config_id = cfg.id;
    } else if (cfg.mode.GetRawMode().hdisplay == kUhdWidth &&
               cfg.mode.GetRawMode().vrefresh == kRefresh30Hz) {
      uhd_30_config_id = cfg.id;
    } else if (cfg.mode.GetRawMode().hdisplay == kFhdWidth) {
      fhd_config_id = cfg.id;
    }
  }
  ASSERT_NE(uhd_60_config_id, 0U);
  ASSERT_NE(uhd_30_config_id, 0U);
  ASSERT_NE(fhd_config_id, 0U);

  // While the external display is inactive (deferred Init() or
  // PowerMode::kOff), QueueConfig() returns kSeamlessNotAllowed for both
  // non-preferred and preferred configs, and
  // ComposerClient::setActiveConfigWithConstraints()'s fallback SetConfig()
  // stages the selected mode in memory with 0 KMS commits without prematurely
  // activating the CRTC.
  EXPECT_EQ(display.SetPowerMode(PowerMode::kOff), HwcDisplay::Error::kNone);
  QueuedConfigTiming timing{};
  EXPECT_EQ(display.QueueConfig(fhd_config_id, /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kSeamlessNotAllowed);
  EXPECT_EQ(display.SetConfig(fhd_config_id), HwcDisplay::ConfigError::kNone);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_FALSE(pipeline->atomic_state_manager->IsActive());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, fhd_config_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, fhd_config_id);

  EXPECT_EQ(display.QueueConfig(uhd_30_config_id, /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kSeamlessNotAllowed);
  EXPECT_EQ(display.SetConfig(uhd_30_config_id),
            HwcDisplay::ConfigError::kNone);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_FALSE(pipeline->atomic_state_manager->IsActive());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, uhd_30_config_id);

  // Selecting the preferred config while inactive also takes the
  // QueueConfig() -> kSeamlessNotAllowed -> SetConfig() staging path with 0 KMS
  // commits, and the subsequent SetPowerMode(PowerMode::kOn) executes a single
  // blocking KMS modeset.
  EXPECT_EQ(display.QueueConfig(uhd_60_config_id, /*desired_time=*/0, &timing),
            HwcDisplay::ConfigError::kSeamlessNotAllowed);
  EXPECT_EQ(display.SetConfig(uhd_60_config_id),
            HwcDisplay::ConfigError::kNone);
  EXPECT_EQ(sink->GetExecuteCommitCount(), 0U);
  EXPECT_FALSE(pipeline->atomic_state_manager->IsActive());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, uhd_60_config_id);
  ASSERT_NE(display.GetLastRequestedConfig(), nullptr);
  EXPECT_EQ(display.GetLastRequestedConfig()->id, uhd_60_config_id);

  EXPECT_EQ(display.SetPowerMode(PowerMode::kOn), HwcDisplay::Error::kNone);
  ASSERT_EQ(sink->GetExecuteCommitCount(), 1U);
  EXPECT_TRUE(pipeline->atomic_state_manager->IsActive());
  ASSERT_NE(display.GetCurrentConfig(), nullptr);
  EXPECT_EQ(display.GetCurrentConfig()->id, uhd_60_config_id);
  const auto& modeset_args = sink->GetCommitHistory().back().front();
  ASSERT_TRUE(modeset_args.display_mode.has_value());
  if (modeset_args.display_mode.has_value()) {
    EXPECT_EQ(modeset_args.display_mode->GetRawMode().hdisplay, kUhdWidth);
    EXPECT_EQ(modeset_args.display_mode->GetRawMode().vdisplay, kUhdHeight);
    EXPECT_EQ(modeset_args.display_mode->GetRawMode().vrefresh, kRefresh60Hz);
  }
}

TEST_F(HwcDisplayTest,
       InitSelectsSupportedDefaultColorModeWhenSrgbUnsupported) {
  class NativeOnlyCapabilities : public BackendDisplayCapabilities {
   public:
    std::optional<std::vector<ColorMode>> GetColorModeOverrides()
        const override {
      return std::vector<ColorMode>{ColorMode::kNative};
    }
  };

  auto pipeline = CreatePipelineWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")});
  ASSERT_NE(pipeline, nullptr);
  pipeline->capabilities = std::make_unique<NativeOnlyCapabilities>();

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  display.SetPipeline(pipeline);

  const auto fb = std::make_shared<FakeFbIdHandle>(kTestFbId);
  display.GetClientLayer().SetLayerProperties(
      CreateFullScreenFhdLayerProperties(fb));
  (void)display.ValidateStagedComposition();
  display.AcceptValidatedComposition();

  auto* sink = fake_device->GetFakeAtomicCommitSink();
  sink->ClearHistory();
  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  EXPECT_TRUE(display.PresentStagedComposition(std::nullopt, present_fence,
                                               release_fences));
  ASSERT_FALSE(sink->GetCommitHistory().empty());
  ASSERT_FALSE(sink->GetCommitHistory().back().empty());
  EXPECT_EQ(sink->GetCommitHistory().back().front().color_mode,
            std::optional<ColorMode>(ColorMode::kNative));
}

TEST_F(HwcDisplayTest, NullEdidWrapperIsHandledGracefullyAcrossDisplayQueries) {
  auto* ext_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(ext_conn, nullptr);
  ext_conn->SetColorspacePropertySupported(true);
  ext_conn->SetEdidBlobData({});
  std::shared_ptr<DrmDisplayPipeline>
      ext_pipeline = fake_device->GetBackend().CreatePipeline(*ext_conn);
  ASSERT_NE(ext_pipeline, nullptr);

  HwcDisplay ext_display(kPrimaryDisplay + 1, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(ext_display.SetPipeline(ext_pipeline));
  EXPECT_EQ(ext_display.GetDisplayBoundsMm(),
            std::make_pair(static_cast<int32_t>(kFakeConnectorMmWidth), -1));
  EXPECT_EQ(ext_display.GetColorModes(),
            std::vector<ColorMode>{ColorMode::kNative});
  EXPECT_FLOAT_EQ(ext_display.GetMaxLuminance(), 500.0F);

  // Also exercise an HDR-capable internal pipeline with forced_color_mode_ set
  // and a null EdidWrapper so InitWcgSupported(), InitHdrSupported(),
  // SetOutputType(OutputType::kSystem), SetHdrOutputMetadata(), and
  // GetHdrCapabilities() all reach their GetEdid() null guards.
  ScopedTestProperty non_existent_dev("vendor.hwc.drm.device",
                                      "/nonexistent_drm_device");
  ScopedTestProperty
      enable_color_pipeline("vendor.hwc.drm.enable_color_pipeline", "1");
  ScopedTestProperty persistent_hdr("vendor.hwc.drm.persistent_hdr", "1");
  ScopedTestProperty force_color_mode("vendor.hwc.drm.force_color_mode", "9");
  ScopedTestProperty bootanim_exit("service.bootanim.exit", "1");
  hwc.GetResMan().Init();

  const auto resources = fake_device->AddPipelineResourcesTyped(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/false);
  auto* int_conn = resources.connector;
  ASSERT_NE(int_conn, nullptr);
  int_conn->SetColorspacePropertySupported(true);
  int_conn->SetHdrOutputMetadataPropertySupported(true);
  int_conn->SetEdidBlobData({});

  auto* fake_crtc = resources.crtc;
  ASSERT_NE(fake_crtc, nullptr);
  fake_crtc->SetCtmPropertySupported(true);
  fake_crtc->SetGammaLutPropertiesSupported(true);

  auto* fake_plane = resources.primary_plane;
  ASSERT_NE(fake_plane, nullptr);
  fake_plane->SetHasColorPipeline(true);

  std::shared_ptr<DrmDisplayPipeline>
      int_pipeline = fake_device->GetBackend().CreatePipeline(*int_conn);
  ASSERT_NE(int_pipeline, nullptr);

  HwcDisplay int_display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(int_display.SetPipeline(int_pipeline));

  std::vector<ui::Hdr> hdr_types{ui::Hdr::HLG};
  constexpr float kUnsetLuminance = -1.0F;
  float max_lum = kUnsetLuminance;
  float max_avg_lum = kUnsetLuminance;
  float min_lum = kUnsetLuminance;
  int_display.GetHdrCapabilities(&hdr_types, &max_lum, &max_avg_lum, &min_lum);
  EXPECT_EQ(hdr_types, std::vector<ui::Hdr>{ui::Hdr::HDR10});
  EXPECT_FLOAT_EQ(max_lum, kUnsetLuminance);
  EXPECT_FLOAT_EQ(max_avg_lum, kUnsetLuminance);
  EXPECT_FLOAT_EQ(min_lum, kUnsetLuminance);

  hwc.GetResMan().DeInit();
}

#if HAS_LIBDISPLAY_INFO
TEST_F(HwcDisplayTest,
       DeinitResetsEdidWrapperWhenSwitchingToConnectorWithoutEdid) {
  constexpr uint16_t kEdidHorizMm = 600;
  constexpr uint16_t kEdidVertMm = 340;
  constexpr uint8_t kEdidWidthCm = 60;
  constexpr uint8_t kEdidHeightCm = 34;

  auto* conn_with_edid = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(conn_with_edid, nullptr);
  conn_with_edid->SetEdidBlobData(MakeEdidWithBounds(kEdidHorizMm, kEdidVertMm,
                                                     kEdidWidthCm,
                                                     kEdidHeightCm));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline_with_edid = fake_device->GetBackend().CreatePipeline(
          *conn_with_edid);
  ASSERT_NE(pipeline_with_edid, nullptr);

  auto* conn_without_edid = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(conn_without_edid, nullptr);
  conn_without_edid->SetEdidBlobData({});
  std::shared_ptr<DrmDisplayPipeline>
      pipeline_without_edid = fake_device->GetBackend().CreatePipeline(
          *conn_without_edid);
  ASSERT_NE(pipeline_without_edid, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(display.SetPipeline(pipeline_with_edid));
  EXPECT_EQ(display.GetDisplayBoundsMm(),
            std::make_pair(int32_t{kEdidHorizMm}, int32_t{kEdidVertMm}));

  ASSERT_TRUE(display.SetPipeline(pipeline_without_edid));
  EXPECT_EQ(display.GetDisplayBoundsMm(),
            std::make_pair(static_cast<int32_t>(kFakeConnectorMmWidth), -1));
}

TEST_F(HwcDisplayTest, GetDisplayBoundsMmReturnsDtdDimensionsWhenPresent) {
  constexpr uint16_t kDtdHorizMm = 600;
  constexpr uint16_t kDtdVertMm = 340;
  constexpr uint8_t kScreenWidthCm = 60;
  constexpr uint8_t kScreenHeightCm = 34;

  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  fake_conn->SetEdidBlobData(MakeEdidWithBounds(kDtdHorizMm, kDtdVertMm,
                                                kScreenWidthCm,
                                                kScreenHeightCm));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(display.SetPipeline(pipeline));
  EXPECT_EQ(display.GetDisplayBoundsMm(),
            std::make_pair(int32_t{kDtdHorizMm}, int32_t{kDtdVertMm}));
}

TEST_F(HwcDisplayTest,
       GetDisplayBoundsMmFallsBackToCtaExtensionDtdWhenBaseDtdSizeIsZero) {
  constexpr uint16_t kCtaHorizMm = 700;
  constexpr uint16_t kCtaVertMm = 390;

  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  fake_conn->SetMmSize(0, 0);
  fake_conn->SetEdidBlobData(
      MakeEdidWithCtaExtensionBounds(/*base_horiz_mm=*/0, /*base_vert_mm=*/0,
                                     /*width_cm=*/0, /*height_cm=*/0,
                                     kCtaHorizMm, kCtaVertMm));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(display.SetPipeline(pipeline));
  EXPECT_EQ(display.GetDisplayBoundsMm(),
            std::make_pair(int32_t{kCtaHorizMm}, int32_t{kCtaVertMm}));
}

TEST_F(HwcDisplayTest,
       GetDisplayBoundsMmFallsBackToScreenSizeWidthWhenDtdSizeIsZero) {
  constexpr uint8_t kScreenWidthCm = 52;
  constexpr uint8_t kScreenHeightCm = 29;
  constexpr int32_t kExpectedWidthMm = 520;

  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  fake_conn->SetMmSize(0, 0);
  fake_conn->SetEdidBlobData(MakeEdidWithBounds(/*horiz_mm=*/0, /*vert_mm=*/0,
                                                kScreenWidthCm,
                                                kScreenHeightCm));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(display.SetPipeline(pipeline));
  EXPECT_EQ(display.GetDisplayBoundsMm(), std::make_pair(kExpectedWidthMm, -1));
}

TEST_F(
    HwcDisplayTest,
    GetDisplayBoundsMmReturnsZeroPairWhenDtdAndScreenSizeAndConnectorAreZero) {
  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  fake_conn->SetMmSize(0, 0);
  fake_conn->SetEdidBlobData(MakeEdidWithBounds(/*horiz_mm=*/0, /*vert_mm=*/0,
                                                /*width_cm=*/0,
                                                /*height_cm=*/0));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(display.SetPipeline(pipeline));
  EXPECT_EQ(display.GetDisplayBoundsMm(), std::make_pair(0, 0));
}

TEST_F(
    HwcDisplayTest,
    GetDisplayBoundsMmFallsBackToConnectorMmWidthWhenEdidIsZeroButConnectorHasSize) {
  constexpr uint32_t kConnectorWidthMm = 480;
  constexpr uint32_t kConnectorHeightMm = 270;

  auto* fake_conn = fake_device->AddPipelineResourcesWithModes(
      {CreateFakeDrmMode(kFhdWidth, kFhdHeight, kRefresh60Hz,
                         DRM_MODE_TYPE_PREFERRED, "1080p60")},
      /*is_external=*/true);
  ASSERT_NE(fake_conn, nullptr);
  fake_conn->SetMmSize(kConnectorWidthMm, kConnectorHeightMm);
  fake_conn->SetEdidBlobData(MakeEdidWithBounds(/*horiz_mm=*/0, /*vert_mm=*/0,
                                                /*width_cm=*/0,
                                                /*height_cm=*/0));
  std::shared_ptr<DrmDisplayPipeline>
      pipeline = fake_device->GetBackend().CreatePipeline(*fake_conn);
  ASSERT_NE(pipeline, nullptr);

  HwcDisplay display(kPrimaryDisplay, /*is_virtual=*/false, &hwc);
  ASSERT_TRUE(display.SetPipeline(pipeline));
  EXPECT_EQ(display.GetDisplayBoundsMm(),
            std::make_pair(static_cast<int32_t>(kConnectorWidthMm), -1));
}
#endif

}  // namespace android::drm_hwcomposer
