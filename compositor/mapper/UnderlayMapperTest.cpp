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

// NOLINTBEGIN(readability-magic-numbers)

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <drm/drm_fourcc.h>

#include <vector>

#include "compositor/CompositorTestUtils.h"
#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "compositor/mapper/UnderlayMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 fourcc_code('P', '0', '1', '0')
#endif

constexpr float kOpaque = 1.0F;
constexpr float kLayerCached = 0.0F;

TEST(UnderlayMapperTest, UnderlayNV12) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // Currently NV12 layers are eligible for underlay.
  HwcLayer underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 0,
                                                             .top = 0,
                                                             .right = 1920,
                                                             .bottom = 1080},
                                                       /*z_order=*/1,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kOpaque,
                                                       DRM_FORMAT_NV12);
  HwcLayer
      not_underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kOpaque,
                                                      DRM_FORMAT_RGBA8888);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_RGBA8888);

  std::vector<LayerMapping> mappings = {{&underlay, CompositionType::kInvalid},
                                        {&not_underlay,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDevice)),
                          AllOf(Field(&LayerMapping::layer, &not_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, UnderlayP010) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // P010 layers are eligible for underlay.
  HwcLayer underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 0,
                                                             .top = 0,
                                                             .right = 1920,
                                                             .bottom = 1080},
                                                       /*z_order=*/1,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kOpaque,
                                                       DRM_FORMAT_P010);
  HwcLayer
      not_underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kOpaque,
                                                      DRM_FORMAT_RGBA8888);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_RGBA8888);

  std::vector<LayerMapping> mappings = {{&underlay, CompositionType::kInvalid},
                                        {&not_underlay,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDevice)),
                          AllOf(Field(&LayerMapping::layer, &not_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, UnderlayNV12ValidatorRejection) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // Currently NV12 layers are eligible for underlay.
  HwcLayer underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 0,
                                                             .top = 0,
                                                             .right = 1920,
                                                             .bottom = 1080},
                                                       /*z_order=*/1,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kOpaque,
                                                       DRM_FORMAT_NV12);
  HwcLayer
      not_underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kOpaque,
                                                      DRM_FORMAT_RGBA8888);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_RGBA8888);

  std::vector<LayerMapping> mappings = {{&underlay, CompositionType::kInvalid},
                                        {&not_underlay,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  // Validator rejection
  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return false;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &not_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, UnderlayRespectSFClientComposition) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // Currently NV12 layers are eligible for underlay.
  // But SF is requesting kClient and that must be respected
  HwcLayer underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 0,
                                                             .top = 0,
                                                             .right = 1920,
                                                             .bottom = 1080},
                                                       /*z_order=*/1,
                                                       CompositionType::kClient,
                                                       /*alpha=*/kOpaque,
                                                       DRM_FORMAT_NV12);
  HwcLayer
      not_underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kOpaque,
                                                      DRM_FORMAT_RGBA8888);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_RGBA8888);

  std::vector<LayerMapping> mappings = {{&underlay, CompositionType::kClient},
                                        {&not_underlay,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kClient)),
                          AllOf(Field(&LayerMapping::layer, &not_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, UnderlayRespectHwcClientComposition) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // Currently NV12 layers are eligible for underlay.
  HwcLayer underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                       IRect{.left = 0,
                                                             .top = 0,
                                                             .right = 1920,
                                                             .bottom = 1080},
                                                       /*z_order=*/1,
                                                       CompositionType::kDevice,
                                                       /*alpha=*/kOpaque,
                                                       DRM_FORMAT_NV12);
  HwcLayer
      not_underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kOpaque,
                                                      DRM_FORMAT_RGBA8888);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_RGBA8888);

  // If underlay is already marked for client composition, the decision should
  // be respected
  std::vector<LayerMapping> mappings = {{&underlay, CompositionType::kClient},
                                        {&not_underlay,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kClient)),
                          AllOf(Field(&LayerMapping::layer, &not_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, NotUnderlayNotPromoted) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // The bottommost layer is not an underlay-eligible
  HwcLayer
      not_underlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/1,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kOpaque,
                                                      DRM_FORMAT_RGBA8888);

  // Underlay-eligible layer is not the bottommost layer
  HwcLayer underlay_eligible = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_NV12);

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_RGBA8888);

  std::vector<LayerMapping> mappings = {{&not_underlay,
                                         CompositionType::kInvalid},
                                        {&underlay_eligible,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &not_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &underlay_eligible),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, HotspotUnderlay) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // The bottom layer is not a video but is a hotspot.
  HwcLayer non_vido_active_underlay = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/1, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  // Layer caching is required for hotspot underlay.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/2,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque,
                                                    DRM_FORMAT_RGBA8888,
                                                    /*is_active=*/false);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached,
                                                      DRM_FORMAT_RGBA8888,
                                                      /*is_active=*/false);

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_BGRA8888,
                                                     /*is_active=*/false);

  std::vector<LayerMapping> mappings =
      {{&non_vido_active_underlay, CompositionType::kInvalid},
       {&non_cached, CompositionType::kInvalid},
       {&cached1, CompositionType::kDeviceOccluded},
       {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer,
                                      &non_vido_active_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDevice)),
                          AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cached1),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, HotspotUnderlayButNoLayerCaching) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // The bottom layer is not a video but is a hotspot.
  HwcLayer non_vido_active_underlay = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/1, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  // Layer caching is required for hotspot underlay, but we don't have one here.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/2,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque,
                                                    DRM_FORMAT_RGBA8888,
                                                    /*is_active=*/false);

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_BGRA8888,
                                                     /*is_active=*/false);

  std::vector<LayerMapping> mappings = {{&non_vido_active_underlay,
                                         CompositionType::kInvalid},
                                        {&non_cached,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer,
                                      &non_vido_active_underlay),
                                Field(&LayerMapping::composition_type,
                                      // not underlay.
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, HotspotUnderlayButOtherActiveLayerPresent) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // The bottom layer is not a video but is a hotspot.
  HwcLayer non_vido_active_underlay = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/1, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  // Layer caching is required for hotspot underlay.
  // But |non_cached| being active ruins hotspot candidacy as all other layers
  // (except for the cursor) are required to be inactive.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/2,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque,
                                                    DRM_FORMAT_RGBA8888,
                                                    // active layer
                                                    /*is_active=*/true);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached,
                                                      DRM_FORMAT_RGBA8888,
                                                      /*is_active=*/false);

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_BGRA8888,
                                                     /*is_active=*/false);

  std::vector<LayerMapping> mappings =
      {{&non_vido_active_underlay, CompositionType::kInvalid},
       {&non_cached, CompositionType::kInvalid},
       {&cached1, CompositionType::kDeviceOccluded},
       {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer,
                                      &non_vido_active_underlay),
                                Field(&LayerMapping::composition_type,
                                      // Not underlay
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cached1),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, HotspotUnderlaysEvenWhenCursorMoves) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // The bottom layer is not a video but is a hotspot.
  HwcLayer non_vido_active_underlay = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/1, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  // Layer caching is required for hotspot underlay.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/2,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque,
                                                    DRM_FORMAT_RGBA8888,
                                                    /*is_active=*/false);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached,
                                                      DRM_FORMAT_RGBA8888,
                                                      /*is_active=*/false);

  // An active cursor should not have an impact on hotspot underlay eligibilty.
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_BGRA8888,
                                                     /*is_active=*/true);

  std::vector<LayerMapping> mappings =
      {{&non_vido_active_underlay, CompositionType::kInvalid},
       {&non_cached, CompositionType::kInvalid},
       {&cached1, CompositionType::kDeviceOccluded},
       {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer,
                                      &non_vido_active_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDevice)),
                          AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cached1),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, HotspotButNotUnderlay) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // Layer caching is required for hotspot underlay.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque,
                                                    DRM_FORMAT_RGBA8888,
                                                    /*is_active=*/false);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached,
                                                      DRM_FORMAT_RGBA8888,
                                                      /*is_active=*/false);

  // Hotspot, but not the bottom layer.
  HwcLayer non_vido_active = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/3, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_BGRA8888,
                                                     /*is_active=*/false);

  std::vector<LayerMapping> mappings =
      {{&non_cached, CompositionType::kInvalid},
       {&cached1, CompositionType::kDeviceOccluded},
       {&non_vido_active, CompositionType::kInvalid},
       {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cached1),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &non_vido_active),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

TEST(UnderlayMapperTest, HotspotUnderlayButClientCompositionRequest) {
  UnderlayMapper mapper;

  MockCompositorDisplay mock_display;

  // The bottom layer is not a video but is a hotspot.
  HwcLayer non_vido_active_underlay = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/1, CompositionType::kDevice,
                  /*alpha=*/kOpaque, DRM_FORMAT_RGBA8888,
                  /*is_active=*/true);

  // Layer caching is required for hotspot underlay.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/2,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque,
                                                    DRM_FORMAT_RGBA8888,
                                                    /*is_active=*/false);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached,
                                                      DRM_FORMAT_RGBA8888,
                                                      /*is_active=*/false);

  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor,
                                                     kOpaque,
                                                     DRM_FORMAT_BGRA8888,
                                                     /*is_active=*/false);

  // Underlay candidate is requested to be client composited.
  std::vector<LayerMapping> mappings =
      {{&non_vido_active_underlay, CompositionType::kClient},
       {&non_cached, CompositionType::kInvalid},
       {&cached1, CompositionType::kDeviceOccluded},
       {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return true;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer,
                                      &non_vido_active_underlay),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kClient)),
                          AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cached1),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}

}  // namespace android::drm_hwcomposer

// NOLINTEND(readability-magic-numbers)
