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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

#include "compositor/CompositorTestUtils.h"
#include "compositor/LayerData.h"
#include "compositor/mapper/LayerCachingMapper.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;

constexpr float kOpaque = 1.0F;
constexpr float kLayerCached = 0.0F;

TEST(LayerCachingMapperTest, CachedLayersOccluded) {
  LayerCachingMapper mapper;

  MockCompositorDisplay mock_display;

  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached);
  HwcLayer cached2 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings = {{&non_cached,
                                         CompositionType::kInvalid},
                                        {&cached1, CompositionType::kInvalid},
                                        {&cached2, CompositionType::kInvalid},
                                        {&cursor, CompositionType::kInvalid}};

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
                          AllOf(Field(&LayerMapping::layer, &cached2),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid))));
}

TEST(LayerCachingMapperTest, CachedLayersOccludedForDeviceOccludedType) {
  LayerCachingMapper mapper;

  MockCompositorDisplay mock_display;

  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer cached1 = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/2, CompositionType::kDeviceOccluded,
                  /*alpha=*/kLayerCached);
  HwcLayer cached2 = CompositorTestUtils::
      CreateLayer(&mock_display,
                  IRect{.left = 0, .top = 0, .right = 1920, .bottom = 1080},
                  /*z_order=*/3, CompositionType::kDeviceOccluded,
                  /*alpha=*/kLayerCached);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings = {{&non_cached,
                                         CompositionType::kInvalid},
                                        {&cached1, CompositionType::kInvalid},
                                        {&cached2, CompositionType::kInvalid},
                                        {&cursor, CompositionType::kInvalid}};

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
                          AllOf(Field(&LayerMapping::layer, &cached2),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid))));
}

TEST(LayerCachingMapperTest, IgnoreNonSFClientCompositionRequest) {
  LayerCachingMapper mapper;

  MockCompositorDisplay mock_display;

  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached);
  HwcLayer cached2 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kDevice,
                                                      /*alpha=*/kLayerCached);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  // As long as SF didn't say the layers should be client composited, HWC should
  // be able to eat through its own client composition determination to device
  // occlude them instead.
  std::vector<LayerMapping> mappings = {{&non_cached,
                                         CompositionType::kInvalid},
                                        {&cached1, CompositionType::kClient},
                                        {&cached2, CompositionType::kClient},
                                        {&cursor, CompositionType::kInvalid}};

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
                          AllOf(Field(&LayerMapping::layer, &cached2),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid))));
}

TEST(LayerCachingMapperTest, RespectSFClientCompositionRequest) {
  LayerCachingMapper mapper;

  MockCompositorDisplay mock_display;

  // Do not override kClient in to device occlusion if SF requested it.
  HwcLayer
      non_cached = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice,
                                                    /*alpha=*/kOpaque);
  HwcLayer cached1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kClient,
                                                      /*alpha=*/kLayerCached);
  HwcLayer cached2 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/3,
                                                      CompositionType::kClient,
                                                      /*alpha=*/kLayerCached);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings = {{&non_cached,
                                         CompositionType::kInvalid},
                                        {&cached1, CompositionType::kClient},
                                        {&cached2, CompositionType::kClient},
                                        {&cursor, CompositionType::kInvalid}};

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
                                      CompositionType::kClient)),
                          AllOf(Field(&LayerMapping::layer, &cached2),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kClient)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid))));
}
}  // namespace android::drm_hwcomposer
