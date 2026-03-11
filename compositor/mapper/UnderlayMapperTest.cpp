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

#include <gtest/gtest.h>

#include <vector>

#include "compositor/CompositorTestUtils.h"
#include "compositor/mapper/UnderlayMapper.h"

#include <drm/drm_fourcc.h>

namespace android::drm_hwcomposer {
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 \
  fourcc_code('P', '0', '1', '0')
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
}  // namespace android::drm_hwcomposer
