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

#include <gtest/gtest.h>

#include <vector>

#include "compositor/CompositorTestUtils.h"
#include "compositor/LayerData.h"
#include "compositor/MtkMappingValidator.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

constexpr float kDefaultSrcRight = 100.0F;
constexpr float kDefaultSrcBottom = 100.0F;
constexpr int kDefaultDstRight = 100;
constexpr int kDefaultDstBottom = 100;

void SetLayerProps(HwcLayer& layer, const FRect& src, const IRect& dst) {
  HwcLayer::LayerProperties props{
      .display_frame = DstRectInfo{.i_rect = dst},
      .source_crop = SrcRectInfo{.f_rect = src},
      .transform = LayerTransform{.rotate90 = false},
  };
  layer.SetLayerProperties(props);
}

// Mtk ValidateMapping allows 1:1, but rejects any scaling
TEST(MtkMappingValidatorTest, ValidMappingAllClient) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  // setup a scaled layer
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, 50, 50});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kClient}};
  EXPECT_TRUE(MtkValidateMapping(mappings));
}

TEST(MtkMappingValidatorTest, ValidMappingNoScaling) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kDefaultDstRight, kDefaultDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_TRUE(MtkValidateMapping(mappings));
}

TEST(MtkMappingValidatorTest, InvalidMappingScalingRequested) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, 50, 50});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(MtkValidateMapping(mappings));
}

}  // namespace
}  // namespace android::drm_hwcomposer

// NOLINTEND(readability-magic-numbers)