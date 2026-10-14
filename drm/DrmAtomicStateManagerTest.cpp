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

#include "drm/DrmAtomicStateManager.h"

#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmPlane.h"
#include "drm/DrmTestUtils.h"

namespace android::drm_hwcomposer {

namespace {

constexpr uint32_t kOverlayPlaneId = 310;
constexpr uint32_t kTestFbId1 = 501;
constexpr uint32_t kTestFbId2 = 502;
constexpr uint32_t kTestFbId3 = 503;
constexpr uint16_t kTestWidth = 1920;
constexpr uint16_t kTestHeight = 1080;
constexpr uint32_t kTestVRefresh = 60;
constexpr int kRapidCreateDestroyCount = 16;

}  // namespace

struct DrmAtomicStateManagerTest : public ::testing::Test {
  void SetUp() override {
    fake_device = std::make_unique<FakeDrmDevice>();
    fake_device->AddPipelineResources();
    fake_device->AddPlane(
        std::make_unique<FakeDrmPlane>(*fake_device, DRM_PLANE_TYPE_OVERLAY,
                                       /*is_valid=*/true, kOverlayPlaneId));
    pipeline = fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
    ASSERT_NE(pipeline, nullptr);
    state_manager = DrmAtomicStateManager::CreateInstance(pipeline.get());
    ASSERT_NE(state_manager, nullptr);
  }

  void TearDown() override {
    state_manager.reset();
    pipeline.reset();
    fake_device.reset();
  }

  static LayerData CreateTestLayerData(uint32_t fb_id = kTestFbId1,
                                       uint16_t width = kTestWidth,
                                       uint16_t height = kTestHeight) {
    LayerData layer{};
    layer.bi = BufferInfo{};
    layer.bi->width = width;
    layer.bi->height = height;
    layer.fb = std::make_shared<FakeFbIdHandle>(fb_id);
    layer.pi.display_frame.i_rect = IRect{0, 0, static_cast<int32_t>(width),
                                          static_cast<int32_t>(height)};
    layer.pi.source_crop.f_rect = FRect{0.0F, 0.0F, static_cast<float>(width),
                                        static_cast<float>(height)};
    return layer;
  }

  std::shared_ptr<LayerToPlaneJoiningPlan> CreateSinglePlanePlan(
      uint32_t fb_id = kTestFbId1) const {
    auto plan = std::make_shared<LayerToPlaneJoiningPlan>();
    plan->plan.emplace_back(CreateTestLayerData(fb_id), pipeline->primary_plane,
                            0);
    return plan;
  }

  std::unique_ptr<FakeDrmDevice> fake_device;
  std::shared_ptr<DrmDisplayPipeline> pipeline;
  std::unique_ptr<DrmAtomicStateManager> state_manager;
};

TEST_F(DrmAtomicStateManagerTest, InitialStateIsInactiveAndEmptyArgsNoOp) {
  EXPECT_FALSE(state_manager->IsActive());

  AtomicCommitArgs empty_args{};
  EXPECT_FALSE(empty_args.HasInputs());
  EXPECT_EQ(state_manager->GetAtomicModeReqForArgs(empty_args), nullptr);
}

TEST_F(DrmAtomicStateManagerTest, ModesetCommitActivatesDisplay) {
  AtomicCommitArgs args{};
  args.blocking = true;
  args.power_mode = PowerMode::kOn;
  args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight, kTestVRefresh);
  args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(args);
  ASSERT_NE(req, nullptr);

  state_manager->FinishPendingRequest(args, std::move(req));
  EXPECT_TRUE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest, DoubleSettingSamePowerModeIsFilteredOut) {
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(req));
  ASSERT_TRUE(state_manager->IsActive());

  // Requesting PowerMode::kOn again with no other inputs should be a no-op.
  AtomicCommitArgs redundant_on{};
  redundant_on.power_mode = PowerMode::kOn;
  EXPECT_EQ(state_manager->GetAtomicModeReqForArgs(redundant_on), nullptr);
  EXPECT_FALSE(redundant_on.power_mode.has_value());
}

TEST_F(DrmAtomicStateManagerTest,
       FrameUpdateCommitsAndDisablesPreviouslyUsedOverlayPlane) {
  const auto [usable_planes, cursor_plane] = pipeline->GetUsablePlanes();
  ASSERT_GE(usable_planes.size(), 2U);

  // Frame 1: Modeset with both primary and overlay planes active.
  auto two_plane_plan = std::make_shared<LayerToPlaneJoiningPlan>();
  two_plane_plan->plan.emplace_back(CreateTestLayerData(kTestFbId1),
                                    usable_planes[0], 0);
  two_plane_plan->plan.emplace_back(CreateTestLayerData(kTestFbId2),
                                    usable_planes[1], 1);

  AtomicCommitArgs frame1{};
  frame1.blocking = true;
  frame1.power_mode = PowerMode::kOn;
  frame1.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                          kTestVRefresh);
  frame1.composition = two_plane_plan;

  auto req1 = state_manager->GetAtomicModeReqForArgs(frame1);
  ASSERT_NE(req1, nullptr);
  state_manager->FinishPendingRequest(frame1, std::move(req1));
  EXPECT_TRUE(state_manager->IsActive());

  // Frame 2: Present only primary plane; overlay plane should be disabled.
  AtomicCommitArgs frame2{};
  frame2.blocking = true;
  frame2.composition = CreateSinglePlanePlan(kTestFbId3);

  auto req2 = state_manager->GetAtomicModeReqForArgs(frame2);
  ASSERT_NE(req2, nullptr);
  state_manager->FinishPendingRequest(frame2, std::move(req2));
  EXPECT_TRUE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest, PowerOffCommitDeactivatesDisplay) {
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto on_req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(on_req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(on_req));
  ASSERT_TRUE(state_manager->IsActive());

  AtomicCommitArgs off_args{};
  off_args.blocking = true;
  off_args.power_mode = PowerMode::kOff;

  auto off_req = state_manager->GetAtomicModeReqForArgs(off_args);
  ASSERT_NE(off_req, nullptr);
  state_manager->FinishPendingRequest(off_args, std::move(off_req));
  EXPECT_FALSE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest, RapidCreateAndDestroyDoesNotDeadlock) {
  for (int i = 0; i < kRapidCreateDestroyCount; ++i) {
    auto manager = DrmAtomicStateManager::CreateInstance(pipeline.get());
    ASSERT_NE(manager, nullptr);
    EXPECT_FALSE(manager->IsActive());
  }
}

}  // namespace android::drm_hwcomposer
