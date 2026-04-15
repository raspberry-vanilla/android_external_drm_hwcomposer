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
#include "compositor/mapper/CursorLayerMapper.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;

bool TrueValidator(const std::vector<LayerMapping>&) {
  return true;
}
}  // namespace

class CursorLayerMapperTest : public testing::TestWithParam<CompositionType> {};

TEST_P(CursorLayerMapperTest, HasCursor) {
  const CompositionType cursor_type = GetParam();
  CursorLayerMapper mapper(cursor_type);

  MockCompositorDisplay mock_display;
  HwcLayer
      non_cursor = CompositorTestUtils::CreateLayer(&mock_display,
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
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings = {{&non_cursor,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kInvalid}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         TrueValidator);

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &non_cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      cursor_type))));
}

TEST_P(CursorLayerMapperTest, NoCursor) {
  const CompositionType cursor_type = GetParam();
  CursorLayerMapper mapper(cursor_type);

  MockCompositorDisplay mock_display;
  HwcLayer
      non_cursor = CompositorTestUtils::CreateLayer(&mock_display,
                                                    IRect{.left = 0,
                                                          .top = 0,
                                                          .right = 1920,
                                                          .bottom = 1080},
                                                    /*z_order=*/1,
                                                    CompositionType::kDevice);

  std::vector<LayerMapping> mappings;
  mappings.push_back({&non_cursor, CompositionType::kInvalid});

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         TrueValidator);

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &non_cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid))));
}

TEST_P(CursorLayerMapperTest, HasCursorButForcedClient) {
  const CompositionType cursor_type = GetParam();
  CursorLayerMapper mapper(cursor_type);

  MockCompositorDisplay mock_display;
  HwcLayer
      non_cursor = CompositorTestUtils::CreateLayer(&mock_display,
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
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings =
      {{&non_cursor, CompositionType::kInvalid},
       // Already forced into client composition.
       {&cursor, CompositionType::kClient}};

  std::vector<LayerMapping> result = mapper.AssignLayers(mappings,
                                                         TrueValidator);

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &non_cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kClient))));
}

TEST_P(CursorLayerMapperTest, ValidatorRejectCursor) {
  const CompositionType cursor_type = GetParam();
  CursorLayerMapper mapper(cursor_type);

  MockCompositorDisplay mock_display;
  HwcLayer
      non_cursor = CompositorTestUtils::CreateLayer(&mock_display,
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
                                                     /*z_order=*/4,
                                                     CompositionType::kCursor);

  std::vector<LayerMapping> mappings = {{&non_cursor,
                                         CompositionType::kInvalid},
                                        {&cursor, CompositionType::kInvalid}};

  std::vector<LayerMapping>
      result = mapper.AssignLayers(mappings,
                                   // Reject any cursor usage
                                   [](const std::vector<LayerMapping>&) {
                                     return false;
                                   });

  EXPECT_THAT(result,
              ElementsAre(AllOf(Field(&LayerMapping::layer, &non_cursor),
                                Field(&LayerMapping::composition_type,
                                      CompositionType::kInvalid)),
                          AllOf(Field(&LayerMapping::layer, &cursor),
                                Field(&LayerMapping::composition_type,
                                      // Cursor is not mapped to cursor
                                      // due to validator rejection.
                                      CompositionType::kInvalid))));
}

INSTANTIATE_TEST_SUITE_P(CursorAndDeviceComposition, CursorLayerMapperTest,
                         testing::Values(CompositionType::kCursor,
                                         CompositionType::kDevice));
}  // namespace android::drm_hwcomposer
