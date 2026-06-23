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

#include <drm/drm_fourcc.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/CompositorTestUtils.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/LayerData.h"
#include "drm/CommitStatus.h"
#include "drm/DrmDevice.h"
#include "drm/DrmPlane.h"
#include "drm/DrmTestUtils.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

using ::testing::_;
using ::testing::Contains;
using ::testing::Field;
using ::testing::Pair;
using ::testing::Return;
using ::testing::ReturnRefOfCopy;

constexpr float kOpaque = 1.0F;
constexpr float kLayerCached = 0.0F;
}  // namespace

TEST(GenericLayerMapperCompositionPlannerTest,
     SingleLayerShouldDeviceComposite) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::CompositionTypeMap{
                {&layer1, CompositionType::kDevice}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     FlatteningForcesClientComposition) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeFlatteningController flatcon;
  flatcon.SetShouldFlatten(true);

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(&flatcon));
  ASSERT_TRUE(flatcon.ShouldFlatten());

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::CompositionTypeMap{
                {&layer1, CompositionType::kClient}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     CtmByGpuForcesClientComposition) {
  GenericLayerMapperCompositionPlanner planner;

  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  // CTM by GPU forces the entire layer stack to GPU composite even if there's
  // only one device-compositable layer and thus should device composite.
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(true));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::CompositionTypeMap{
                {&layer1, CompositionType::kClient}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     GpuScalingRequiredForcesClientcomposition) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer scaled_underlay_candidate(&mock_display);
  {
    uint32_t width = 1920;
    uint32_t height = 1080;

    HwcLayer::LayerProperties props{
        .buffer = HwcLayer::Buffer{.bi = BufferInfo{.width = width,
                                                    .height = height,
                                                    // NV12 is an underlay
                                                    // candidate.
                                                    .format = DRM_FORMAT_NV12},
                                   .fb = std::make_shared<MockFbIdHandle>()},
        .composition_type = CompositionType::kDevice,
        .display_frame = DstRectInfo{.i_rect = IRect{.left = 0,
                                                     .top = 0,
                                                     .right = static_cast<
                                                         int32_t>(width),
                                                     .bottom = static_cast<
                                                         int32_t>(height)}},
        .alpha = kOpaque,
        .source_crop = SrcRectInfo{.f_rect = FRect{.left = 0.0F,
                                                   .top = 0.0F,
                                                   // Scaling required.
                                                   .right = width / 2.0F,
                                                   .bottom = height / 2.0F}},
        .z_order = 0,
    };
    scaled_underlay_candidate.SetLayerProperties(props);
  }

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&scaled_underlay_candidate}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  // |scaled_underlay_candidate| requires scaling due to its source crop, and
  // with ForcedScalingWithGpu() being true the layer is going to be forced into
  // GPU composition.
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(true));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::CompositionTypeMap{
                {&scaled_underlay_candidate, CompositionType::kClient}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest, SingleLayerAndCursor) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/1,
                                                     CompositionType::kCursor);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&layer1, CompositionType::kDevice},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     SingleLayerAndCursorNonIdentityCTMFallback) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/1,
                                                     CompositionType::kCursor);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  // Cursor with non-identity color transform should use the device plane
  // instead.
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(true));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&layer1, CompositionType::kDevice},
                                    {&cursor, CompositionType::kDevice}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     SingleLayerAndCursorInvalidCursorLayerFallback) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/1,
                                                     CompositionType::kCursor);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  // Have the cursor layer reject the layer data from the cursor layer.
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = false;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  ON_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillByDefault(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&layer1, CompositionType::kDevice},
                                    {&cursor, CompositionType::kDevice}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     SingleLayerAndCursorTestFailFallback) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/1,
                                                     CompositionType::kCursor);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  // Cursor should be valid for the layer, but the test commit should trigger
  // the fallback.
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = false;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  ON_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillByDefault(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  // Any test compositions mapping the cursor to the cursor plane should fail.
  EXPECT_CALL(mock_display,
              TestComposition(
                  Field(&CompositionPlanner::ValidatedComposition::
                            composition_types,
                        Contains(Pair(&cursor, CompositionType::kCursor)))))
      .WillRepeatedly(Return(CommitStatus::InternalFailure()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // The cursor layer should be mapped to the device layer instead.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&layer1, CompositionType::kDevice},
                                    {&cursor, CompositionType::kDevice}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     SingleLayerAndCursorTestFailAll) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 1920,
                                                           .bottom = 1080},
                                                     /*z_order=*/0,
                                                     CompositionType::kDevice);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/1,
                                                     CompositionType::kCursor);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&layer1, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  // Cursor should be valid for the layer, but the test commit should trigger
  // the fallback.
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = false;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  ON_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillByDefault(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  // All test compositions should fail.
  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::InternalFailure()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // The cursor layer should be mapped to client instead.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&layer1, CompositionType::kClient},
                                    {&cursor, CompositionType::kClient}}));
  EXPECT_FALSE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest, LayerCachingDeviceOcclusion) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  // Task bar and window are cached (alpha of 0.0F).
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      status_bar = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 52},
                                                    /*z_order=*/3,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer task_bar_cached = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 920, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kDevice,
                  /*alpha=*/kLayerCached);
  HwcLayer
      window_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 300,
                                                             .top = 300,
                                                             .right = 900,
                                                             .bottom = 900},
                                                       /*z_order=*/1,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kLayerCached);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/0,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&wallpaper, &window_cached,
                                                    &task_bar_cached,
                                                    &status_bar, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // Cached layers (window, task bar) are supposed to be device occluded if
  // possible.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&wallpaper, CompositionType::kClient},
                                    {&window_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&task_bar_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&status_bar, CompositionType::kClient},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     NoDeviceOcclusionForCachedClientLayer) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  // Task bar and window are cached, but are marked as client composited, which
  // must be respected.
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      status_bar = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 52},
                                                    /*z_order=*/3,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer task_bar_cached = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 920, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kClient,
                  /*alpha=*/kLayerCached);
  HwcLayer
      window_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 300,
                                                             .top = 300,
                                                             .right = 900,
                                                             .bottom = 900},
                                                       /*z_order=*/1,
                                                       CompositionType::kClient,
                                                       /*alpha=*/kLayerCached);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/0,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(Return(std::vector<const HwcLayer*>{&wallpaper, &window_cached,
                                                    &task_bar_cached,
                                                    &status_bar, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // Cached layers (window, task bar) are marked kClient by SF and must be
  // respected.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&wallpaper, CompositionType::kClient},
                                    {&window_cached, CompositionType::kClient},
                                    {&task_bar_cached,
                                     CompositionType::kClient},
                                    {&status_bar, CompositionType::kClient},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest, Underlay) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      status_bar = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 52},
                                                    /*z_order=*/3,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/0,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);
  // Underlay candidates are always sent at the bottom of the layer stack.
  HwcLayer underlayed_nv12 = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 400, .top = 400, .right = 900, .bottom = 900},
                  /*z_order=*/0, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_NV12);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&underlayed_nv12, &wallpaper,
                                              &status_bar, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // Underlay layer should be device composited.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&underlayed_nv12,
                                     CompositionType::kDevice},
                                    {&wallpaper, CompositionType::kClient},
                                    {&status_bar, CompositionType::kClient},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest, AttemptUnderlayButIneligible) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      status_bar = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 52},
                                                    /*z_order=*/3,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/0,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);
  // Underlay candidates are always sent at the bottom of the layer stack.
  // Set type to kClient so that it is inelibile for device composition.
  HwcLayer underlayed_nv12 = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 400, .top = 400, .right = 900, .bottom = 900},
                  /*z_order=*/0, CompositionType::kClient,
                  /*alpha=*/kOpaque, DRM_FORMAT_NV12);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&underlayed_nv12, &wallpaper,
                                              &status_bar, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // Underlay layer should be forced into client composition.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&underlayed_nv12,
                                     CompositionType::kClient},
                                    {&wallpaper, CompositionType::kClient},
                                    {&status_bar, CompositionType::kClient},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest, UnderlayAndLayerCached) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  // Task bar and window are cached.
  // Underlay eligible for device composition.
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      status_bar = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 52},
                                                    /*z_order=*/3,
                                                    CompositionType::kClient,
                                                    /*alpha=*/kOpaque);
  HwcLayer task_bar_cached = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 920, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kDevice,
                  /*alpha=*/kLayerCached);
  HwcLayer
      window_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 300,
                                                             .top = 300,
                                                             .right = 900,
                                                             .bottom = 900},
                                                       /*z_order=*/2,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kLayerCached);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/1,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);
  HwcLayer underlayed_nv12 = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 400, .top = 400, .right = 900, .bottom = 900},
                  /*z_order=*/0, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_NV12);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&underlayed_nv12, &wallpaper,
                                              &window_cached, &task_bar_cached,
                                              &status_bar, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&underlayed_nv12,
                                     CompositionType::kDevice},
                                    {&wallpaper, CompositionType::kClient},
                                    {&window_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&task_bar_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&status_bar, CompositionType::kClient},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest, HotspotUnderlayAndLayerCached) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  // Task bar and window are cached.
  // Underlay eligible for device composition.
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      status_bar = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 52},
                                                    /*z_order=*/3,
                                                    CompositionType::kClient,
                                                    /*alpha=*/kOpaque);
  HwcLayer task_bar_cached = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 920, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kDevice,
                  /*alpha=*/kLayerCached);
  HwcLayer
      window_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 300,
                                                             .top = 300,
                                                             .right = 900,
                                                             .bottom = 900},
                                                       /*z_order=*/2,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kLayerCached);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/1,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);
  HwcLayer underlayed_hotspot = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 400, .top = 400, .right = 900, .bottom = 900},
                  /*z_order=*/0, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&underlayed_hotspot, &wallpaper,
                                              &window_cached, &task_bar_cached,
                                              &status_bar, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&underlayed_hotspot,
                                     CompositionType::kDevice},
                                    {&wallpaper, CompositionType::kClient},
                                    {&window_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&task_bar_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&status_bar, CompositionType::kClient},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     LeftoverLayerWithCachedLayersShouldDeviceComposite) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  // Status bar, task bar and window are cached (alpha of 0.0F).
  // Wallpaper and cursor are not cached.
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer status_bar_cached = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 52},
                  /*z_order=*/3, CompositionType::kDevice,
                  /*alpha=*/kLayerCached);
  HwcLayer task_bar_cached = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 920, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kDevice,
                  /*alpha=*/kLayerCached);
  HwcLayer
      window_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 300,
                                                             .top = 300,
                                                             .right = 900,
                                                             .bottom = 900},
                                                       /*z_order=*/1,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kLayerCached);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/0,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&wallpaper, &window_cached,
                                              &task_bar_cached,
                                              &status_bar_cached, &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(4));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // Cached layers (status bar, window, task bar) are supposed to be device
  // occluded if possible. Since there is only one non-cached, non-device
  // composited layer (the wallper), it should be device composited to avoid GPU
  // composition of a single layer.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&wallpaper, CompositionType::kDevice},
                                    {&window_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&task_bar_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&status_bar_cached,
                                     CompositionType::kDeviceOccluded},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}

TEST(GenericLayerMapperCompositionPlannerTest,
     LeftoverLayerJustEnoughPlanes) {
  GenericLayerMapperCompositionPlanner planner;
  MockCompositorDisplay mock_display;

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);
  HwcLayer
      wallpaper = CompositorTestUtils::CreateLayer(&mock_display,
                                                   IRect{.left = 0,
                                                         .top = 0,
                                                         .right = 1920,
                                                         .bottom = 1080},
                                                   /*z_order=*/0,
                                                   CompositionType::kDevice,
                                                   /*alpha=*/kOpaque);
  // Underlay candidates are always sent at the bottom of the layer stack.
  HwcLayer underlayed_nv12 = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 400, .top = 400, .right = 900, .bottom = 900},
                  /*z_order=*/0, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_NV12);

  EXPECT_CALL(mock_display, GetOrderLayersByZPos())
      .WillOnce(
          Return(std::vector<const HwcLayer*>{&underlayed_nv12, &wallpaper,
                                              &cursor}));

  EXPECT_CALL(mock_display, GetLastPresentedComposition())
      .WillRepeatedly(ReturnRefOfCopy(PresentedCompositionCache()));

  EXPECT_CALL(mock_display, GetFlatCon()).WillRepeatedly(Return(nullptr));
  EXPECT_CALL(mock_display, CtmByGpu()).WillRepeatedly(Return(false));
  EXPECT_CALL(mock_display, ForcedScalingWithGpu())
      .WillRepeatedly(Return(false));

  FakeDrmDevice device;
  std::shared_ptr<FakeDrmPlane>
      cursor_plane = std::make_shared<FakeDrmPlane>(device,
                                                    DRM_PLANE_TYPE_CURSOR);
  cursor_plane->is_valid_ = true;

  // Just enough planes to fit both layers onto overlay.
  EXPECT_CALL(mock_display, GetNumAvailablePlanes()).WillRepeatedly(Return(2));
  EXPECT_CALL(mock_display, GetCursorPlane())
      .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
  EXPECT_CALL(mock_display, CursorPlaneNeedsColorPipeline(_))
      .WillOnce(Return(false));

  EXPECT_CALL(mock_display, TestComposition(_))
      .WillRepeatedly(Return(CommitStatus::Success()));

  auto [composition, _] = planner.ValidateDisplay(&mock_display);

  // Underlay layer should be device composited.
  // Leftover wallpaper layer should be device composited.
  EXPECT_EQ(composition.composition_types,
            (CompositionPlanner::
                 CompositionTypeMap{{&underlayed_nv12,
                                     CompositionType::kDevice},
                                    {&wallpaper, CompositionType::kDevice},
                                    {&cursor, CompositionType::kCursor}}));
  EXPECT_TRUE(composition.cursor_plane_validated);
}
}  // namespace android::drm_hwcomposer
