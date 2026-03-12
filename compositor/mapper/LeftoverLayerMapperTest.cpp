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
#include "compositor/mapper/LeftoverLayerMapper.h"

namespace android::drm_hwcomposer {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;

constexpr float kOpaque = 1.0F;
constexpr float kLayerCached = 0.0F;

bool TrueValidator(const std::vector<LayerMapping>&) {
  return true;
}
}  // namespace

TEST(LeftoverLayerMapperTest, SingleInvalidIsDeviceComposited) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice);

  std::vector<LayerMapping> mappings = {{&layer, CompositionType::kInvalid}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kDevice))));
}

TEST(LeftoverLayerMapperTest, SingleClientNotDeviceComposited) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kClient);

  std::vector<LayerMapping> mappings = {{&layer, CompositionType::kClient}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kClient))));
}

TEST(LeftoverLayerMapperTest, SingleInvalidButNegativeValidator) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice);

  std::vector<LayerMapping> mappings = {{&layer, CompositionType::kInvalid}};

  // Validator always returns false.
  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   [](const std::vector<LayerMapping>&) {
                                     return false;
                                   });

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kInvalid))));
}

TEST(LeftoverLayerMapperTest, SingleLayerAndCursor) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice);
  HwcLayer cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                     IRect{.left = 0,
                                                           .top = 0,
                                                           .right = 32,
                                                           .bottom = 32},
                                                     /*z_order=*/2,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings = {{&layer, CompositionType::kInvalid},
                                        {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kDevice)),
                                  AllOf(Field(&LayerMapping::layer, &cursor),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kCursor))));
}

TEST(LeftoverLayerMapperTest, SingleLayerAndOverlay) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice);
  HwcLayer overlay = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/2,
                                                      CompositionType::kDevice);

  std::vector<LayerMapping> mappings = {{&layer, CompositionType::kInvalid},
                                        {&overlay, CompositionType::kDevice}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kDevice)),
                                  AllOf(Field(&LayerMapping::layer, &overlay),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kDevice))));
}

TEST(LeftoverLayerMapperTest, TwoLayersNotEligible) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer_1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/1,
                                                      CompositionType::kDevice);
  HwcLayer layer_2 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/1,
                                                      CompositionType::kDevice);

  std::vector<LayerMapping> mappings = {{&layer_1, CompositionType::kInvalid},
                                        {&layer_2, CompositionType::kInvalid}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer_1),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kInvalid)),
                                  AllOf(Field(&LayerMapping::layer, &layer_2),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kInvalid))));
}

TEST(LeftoverLayerMapperTest, OneInvalidOneClient) {
  LeftoverLayerMapper mapper;

  MockCompositorDisplay mock_display;
  HwcLayer layer_1 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/1,
                                                      CompositionType::kDevice);
  HwcLayer layer_2 = CompositorTestUtils::CreateLayer(&mock_display,
                                                      IRect{.left = 0,
                                                            .top = 0,
                                                            .right = 1920,
                                                            .bottom = 1080},
                                                      /*z_order=*/1,
                                                      CompositionType::kDevice);

  std::vector<LayerMapping> mappings = {{&layer_1, CompositionType::kInvalid},
                                        {&layer_2, CompositionType::kClient}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result, ElementsAre(AllOf(Field(&LayerMapping::layer, &layer_1),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kInvalid)),
                                  AllOf(Field(&LayerMapping::layer, &layer_2),
                                        Field(&LayerMapping::composition_type,
                                              CompositionType::kClient))));
}

TEST(LeftoverLayerMapperTest, LayerCaching) {
  LeftoverLayerMapper mapper;
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

  std::vector<LayerMapping> mappings =
      {{&non_cached, CompositionType::kInvalid},
       {&cached1, CompositionType::kDeviceOccluded},
       {&cached2, CompositionType::kDeviceOccluded},
       {&cursor, CompositionType::kCursor}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         &TrueValidator);

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &non_cached),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDevice)),
                          AllOf(Field(&LayerMapping::layer, &cached1),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cached2),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kDeviceOccluded)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kCursor))));
}
}  // namespace android::drm_hwcomposer
