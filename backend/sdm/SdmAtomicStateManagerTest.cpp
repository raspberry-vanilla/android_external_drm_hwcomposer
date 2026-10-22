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

#include "backend/sdm/SdmAtomicStateManager.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <core/buffer_allocator.h>  // IWYU pragma: keep
#include <core/display_interface.h>
#include <core/sdm_types.h>
#include <core/socket_handler.h>              // IWYU pragma: keep
#include <sdm_compositor_cb_intf.h>           // IWYU pragma: keep
#include <sdm_compositor_sideband_cb_intf.h>  // IWYU pragma: keep
#include <sdm_display_intf_drawcycle.h>
#include <sdm_display_intf_lifecycle.h>
#include <sdm_display_intf_settings.h>
#include <utils/fence.h>

#include "backend/sdm/SnapAllocHandle.h"
#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmMode.h"
#include "hwc/HwcDisplay.h"

namespace android::drm_hwcomposer {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::SetArgPointee;

constexpr uint64_t kTestSdmDisplayId = 52;
constexpr uint32_t kTestSdmConfigId = 1;
constexpr uint16_t kTestWidth = 1920;
constexpr uint16_t kTestHeight = 1080;
constexpr uint32_t kTestFps = 60;
constexpr uint32_t kTestVsyncPeriodNs = 16666666;
constexpr uint16_t kTestHBlank = 160;
constexpr uint16_t kTestVBlank = 45;
constexpr uint32_t kTestKhzDivisor = 1000;

class MockSdmLifeCycleIntf : public sdm::SDMDisplayLifeCycleIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, Init,
              (sdm::BufferAllocator*, sdm::SocketHandler*,
               sdm::DebugCallbackIntf*),
              (override));
  MOCK_METHOD(void, RegisterCompositorCallback,
              (sdm::SDMCompositorCbIntf*, bool), (override));
  MOCK_METHOD(void, RegisterSideBandCallback,
              (sdm::SDMSideBandCompositorCbIntf*, bool), (override));
  MOCK_METHOD(sdm::DisplayError, SetPowerMode,
              (uint64_t disp_id, int32_t power_mode), (override));
};

class MockSdmDrawCycleIntf : public sdm::SDMDisplayDrawCycleIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, SetActiveConfigIndex,
              (uint64_t disp_id, uint32_t index), (override));
  MOCK_METHOD(sdm::DisplayError, PresentDisplay,
              (uint64_t disp_id, std::shared_ptr<sdm::Fence>* out_retire_fence),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetClientTarget,
              (uint64_t disp_id, const sdm::SnapHandle* target,
               std::shared_ptr<sdm::Fence> acquire_fence, int32_t dataspace,
               sdm::SDMRegion damage, uint32_t version),
              (override));
  MOCK_METHOD(sdm::DisplayError, WaitForDrawCycleToComplete, (uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, LayerStackUpdated, (uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, CommitOrPrepare,
              (uint64_t disp_id, bool validate_only,
               std::shared_ptr<sdm::Fence>* out_retire_fence,
               uint32_t* out_num_types, uint32_t* out_num_requests,
               bool* out_needs_commit),
              (override));
  MOCK_METHOD(sdm::DisplayError, AcceptDisplayChanges, (uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, GetChangedCompositionTypes,
              (uint64_t disp_id, uint32_t* out_num_elements,
               int64_t* out_layers, int32_t* out_types),
              (override));
  MOCK_METHOD(sdm::DisplayError, GetDisplayRequests,
              (uint64_t disp_id, int32_t* out_display_requests,
               uint32_t* out_num_elements, sdm::LayerId* out_layers,
               int32_t* out_layer_requests),
              (override));
};

class MockSdmSettingsIntf : public sdm::SDMDisplaySettingsIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, SetIdleTimeout, (uint32_t timeout_ms),
              (override));
  MOCK_METHOD(sdm::DisplayError, GetColorModes,
              (uint64_t disp_id, uint32_t* out_num_modes, int32_t* out_modes),
              (override));
  MOCK_METHOD(
      sdm::DisplayError, GetAllDisplayAttributes,
      (uint64_t disp_id,
       (std::map<uint32_t, sdm::DisplayConfigVariableInfo>*)out_configs),
      (override));
  MOCK_METHOD(sdm::DisplayError, GetActiveConfig,
              (uint64_t disp_id, uint32_t* out_config_id), (override));
  MOCK_METHOD(sdm::DisplayError, SetActiveConfig,
              (uint64_t disp_id, uint32_t config_id), (override));
  MOCK_METHOD(sdm::DisplayError, SetColorModeWithRenderIntent,
              (uint64_t disp_id, int32_t color_mode, int32_t render_intent),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetColorTransform,
              (uint64_t disp_id, const std::vector<float>& matrix), (override));
  MOCK_METHOD(int, GetDisplayConfigGroup,
              (uint64_t disp_id, const sdm::DisplayConfigVariableInfo& config),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetDisplayBrightnessNits,
              (uint64_t disp_id, float brightness_nits), (override));
};

DrmMode MakeTestDrmMode(uint16_t width, uint16_t height, uint32_t vrefresh_hz) {
  drmModeModeInfo raw_mode{};
  raw_mode.hdisplay = width;
  raw_mode.vdisplay = height;
  raw_mode.vrefresh = vrefresh_hz;
  raw_mode.htotal = width + kTestHBlank;
  raw_mode.vtotal = height + kTestVBlank;
  raw_mode.clock = (raw_mode.htotal * raw_mode.vtotal * vrefresh_hz) /
                   kTestKhzDivisor;
  return DrmMode(&raw_mode);
}

struct SdmAtomicStateManagerTest : public ::testing::Test {
  void InitManagerWithNativeColorMode() {
    ON_CALL(settings_intf, GetColorModes(kTestSdmDisplayId, _, nullptr))
        .WillByDefault(DoAll(SetArgPointee<1>(1U), Return(sdm::kErrorNone)));
    ON_CALL(settings_intf, GetColorModes(kTestSdmDisplayId, _, NotNull()))
        .WillByDefault(
            DoAll(SetArgPointee<1>(1U),
                  SetArgPointee<2>(static_cast<int32_t>(ColorMode::kNative)),
                  Return(sdm::kErrorNone)));

    std::map<uint32_t, sdm::DisplayConfigVariableInfo> configs;
    sdm::DisplayConfigVariableInfo cfg{};
    cfg.x_pixels = kTestWidth;
    cfg.y_pixels = kTestHeight;
    cfg.fps = kTestFps;
    cfg.vsync_period_ns = kTestVsyncPeriodNs;
    configs[kTestSdmConfigId] = cfg;

    ON_CALL(settings_intf, GetAllDisplayAttributes(kTestSdmDisplayId, _))
        .WillByDefault(
            DoAll(SetArgPointee<1>(configs), Return(sdm::kErrorNone)));
    ON_CALL(settings_intf, GetActiveConfig(kTestSdmDisplayId, _))
        .WillByDefault(
            DoAll(SetArgPointee<1>(kTestSdmConfigId), Return(sdm::kErrorNone)));

    manager = std::make_unique<SdmAtomicStateManager>(kTestSdmDisplayId,
                                                      &lifecycle_intf,
                                                      &draw_cycle_intf,
                                                      &settings_intf);
  }

  NiceMock<MockSdmLifeCycleIntf> lifecycle_intf;
  NiceMock<MockSdmDrawCycleIntf> draw_cycle_intf;
  NiceMock<MockSdmSettingsIntf> settings_intf;
  std::unique_ptr<SdmAtomicStateManager> manager;
};

// Verifies that an initial modeset commit (setting both display_mode and
// power_mode = kOn) that also carries a client-composited frame configures the
// SDM active config and power mode, uploads the client target, and presents the
// display in a single atomic commit.
TEST_F(SdmAtomicStateManagerTest,
       StagedPowerOnModesetFrameWithClientTargetCallsPresentDisplay) {
  InitManagerWithNativeColorMode();

  auto plan = std::make_shared<LayerToPlaneJoiningPlan>();
  LayerData client_layer{};
  client_layer.bi = BufferInfo{};
  client_layer.bi->fds_shared = SnapAllocHandle::Create(nullptr);
  plan->plan.emplace_back(std::move(client_layer), nullptr, 0);
  plan->client_z_order = 0;

  AtomicCommitArgs args{};
  args.display_mode = MakeTestDrmMode(kTestWidth, kTestHeight, kTestFps);
  args.power_mode = PowerMode::kOn;
  args.seamless = false;
  args.blocking = true;
  args.composition = plan;

  {
    InSequence seq;
    EXPECT_CALL(settings_intf,
                SetActiveConfig(kTestSdmDisplayId, kTestSdmConfigId))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
    EXPECT_CALL(lifecycle_intf,
                SetPowerMode(kTestSdmDisplayId,
                             static_cast<int32_t>(
                                 sdm::SDMPowerMode::POWER_MODE_ON)))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
    EXPECT_CALL(draw_cycle_intf,
                SetClientTarget(kTestSdmDisplayId, nullptr, _, _, _, _))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
    EXPECT_CALL(draw_cycle_intf, PresentDisplay(kTestSdmDisplayId, _))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
  }

  auto result = manager->ExecuteAtomicCommit(args);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(manager->IsActive());
}

// Verifies that on an already powered-on display, a steady-state frame commit
// with a client target updates the SDM client target and presents the
// composition without re-applying power or display mode settings.
TEST_F(SdmAtomicStateManagerTest,
       ActiveFrameWithClientTargetPresentsComposition) {
  InitManagerWithNativeColorMode();

  AtomicCommitArgs power_on_args{};
  power_on_args.power_mode = PowerMode::kOn;
  ON_CALL(lifecycle_intf, SetPowerMode(kTestSdmDisplayId, _))
      .WillByDefault(Return(sdm::kErrorNone));
  ASSERT_TRUE(manager->ExecuteAtomicCommit(power_on_args).has_value());

  auto plan = std::make_shared<LayerToPlaneJoiningPlan>();
  LayerData client_layer{};
  client_layer.bi = BufferInfo{};
  client_layer.bi->fds_shared = SnapAllocHandle::Create(nullptr);
  plan->plan.emplace_back(std::move(client_layer), nullptr, 0);
  plan->client_z_order = 0;

  AtomicCommitArgs frame_args{};
  frame_args.composition = plan;

  {
    InSequence seq;
    EXPECT_CALL(draw_cycle_intf,
                SetClientTarget(kTestSdmDisplayId, nullptr, _, _, _, _))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
    EXPECT_CALL(draw_cycle_intf, PresentDisplay(kTestSdmDisplayId, _))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
  }

  auto result = manager->ExecuteAtomicCommit(frame_args);
  EXPECT_TRUE(result.has_value());
}

// Verifies that if SDM rejects SetColorModeWithRenderIntent (e.g. with
// kErrorNotSupported), ExecuteAtomicCommit logs the error and continues
// presenting the frame rather than aborting the commit, and caches
// last_color_mode_ so the rejected mode is not retried on every frame.
TEST_F(SdmAtomicStateManagerTest,
       SetColorModeFailureDoesNotAbortCommitOrRetrySameMode) {
  InitManagerWithNativeColorMode();

  AtomicCommitArgs power_on_args{};
  power_on_args.power_mode = PowerMode::kOn;
  ON_CALL(lifecycle_intf, SetPowerMode(kTestSdmDisplayId, _))
      .WillByDefault(Return(sdm::kErrorNone));
  ASSERT_TRUE(manager->ExecuteAtomicCommit(power_on_args).has_value());

  auto plan = std::make_shared<LayerToPlaneJoiningPlan>();
  LayerData client_layer{};
  client_layer.bi = BufferInfo{};
  client_layer.bi->fds_shared = SnapAllocHandle::Create(nullptr);
  plan->plan.emplace_back(std::move(client_layer), nullptr, 0);
  plan->client_z_order = 0;

  AtomicCommitArgs args{};
  args.color_mode = ColorMode::kSrgb;
  args.composition = plan;

  {
    InSequence seq;
    EXPECT_CALL(settings_intf,
                SetColorModeWithRenderIntent(kTestSdmDisplayId,
                                             static_cast<int32_t>(
                                                 ColorMode::kSrgb),
                                             0))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNotSupported));
    EXPECT_CALL(draw_cycle_intf,
                SetClientTarget(kTestSdmDisplayId, nullptr, _, _, _, _))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
    EXPECT_CALL(draw_cycle_intf, PresentDisplay(kTestSdmDisplayId, _))
        .Times(1)
        .WillOnce(Return(sdm::kErrorNone));
  }

  EXPECT_TRUE(manager->ExecuteAtomicCommit(args).has_value());

  EXPECT_CALL(settings_intf, SetColorModeWithRenderIntent(_, _, _)).Times(0);
  EXPECT_CALL(draw_cycle_intf,
              SetClientTarget(kTestSdmDisplayId, nullptr, _, _, _, _))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));
  EXPECT_CALL(draw_cycle_intf, PresentDisplay(kTestSdmDisplayId, _))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));
  EXPECT_TRUE(manager->ExecuteAtomicCommit(args).has_value());
}

TEST_F(SdmAtomicStateManagerTest,
       ClientTargetWithNullSnapAllocHandleFailsWithoutPresentDisplay) {
  InitManagerWithNativeColorMode();

  AtomicCommitArgs power_on_args{};
  power_on_args.power_mode = PowerMode::kOn;
  ON_CALL(lifecycle_intf, SetPowerMode(kTestSdmDisplayId, _))
      .WillByDefault(Return(sdm::kErrorNone));
  ASSERT_TRUE(manager->ExecuteAtomicCommit(power_on_args).has_value());

  auto plan = std::make_shared<LayerToPlaneJoiningPlan>();
  LayerData client_layer{};
  client_layer.bi = BufferInfo{};
  client_layer.bi->fds_shared = nullptr;
  plan->plan.emplace_back(std::move(client_layer), nullptr, 0);
  plan->client_z_order = 0;

  AtomicCommitArgs frame_args{};
  frame_args.composition = plan;

  EXPECT_CALL(draw_cycle_intf, SetClientTarget(_, _, _, _, _, _)).Times(0);
  EXPECT_CALL(draw_cycle_intf, PresentDisplay(_, _)).Times(0);

  EXPECT_FALSE(manager->ExecuteAtomicCommit(frame_args).has_value());
}
}  // namespace
}  // namespace android::drm_hwcomposer
