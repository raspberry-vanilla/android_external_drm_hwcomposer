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

#include "compositor/CompositorTestUtils.h"
#include "compositor/IntelMappingValidator.h"
#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "drm/DrmTestUtils.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

constexpr float kDefaultSrcRight = 100.0F;
constexpr float kDefaultSrcBottom = 100.0F;
constexpr int kDefaultDstRight = 100;
constexpr int kDefaultDstBottom = 100;

constexpr int kScaledDstRight = 50;
constexpr int kScaledDstBottom = 50;

constexpr float kOddSrcLeft = 1.0F;
constexpr float kOddSrcRight = 101.0F;
constexpr float kOddWidthSrcRight = 99.0F;

constexpr float kMaxSrcWidthExceededRight = 4098.0F;
constexpr float kMaxSrcHeightExceededBottom = 8194.0F;
constexpr int kMaxDstWidthExceededRight = 8194;
constexpr int kMaxDstHeightExceededBottom = 8194;

static void SetLayerProps(HwcLayer& layer, const FRect& src, const IRect& dst,
                          bool rotate90 = false) {
  HwcLayer::LayerProperties props{
      .display_frame = DstRectInfo{.i_rect = dst},
      .source_crop = SrcRectInfo{.f_rect = src},
      .transform = LayerTransform{.rotate90 = rotate90},
  };
  layer.SetLayerProperties(props);
}

// This test suite validates the behavior of IntelValidateMapping() function,
// which checks if a given set of layer mappings can be supported by Intel
// hardware. The tests cover various scenarios including valid and invalid
// mappings based on rotation, scaling, and hardware limitations.

// This test verifies that a mapping with all layers set to client composition
// is considered valid, even if the layers have properties that would normally
// make them invalid for device composition.
TEST(IntelMappingValidatorTest, ValidMapping_AllClient) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  // Setup a layer that would fail if evaluated as a device layer (e.g., has
  // rotation)
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kDefaultDstRight, kDefaultDstBottom},
                /*rotate90=*/true);

  // But mapped as client, so scaling and rotation checks should be skipped
  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kClient}};
  EXPECT_TRUE(IntelValidateMapping(mappings));
}

// This test verifies that a standard 1:1 layer mapping without any scaling or
// rotation is considered valid.
TEST(IntelMappingValidatorTest, ValidMapping_NoScalingOrRotation) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kDefaultDstRight, kDefaultDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_TRUE(IntelValidateMapping(mappings));
}

// This test ensures that any mapping requiring a 90-degree rotation is marked
// as invalid, as it is unsupported by the Intel hardware.
TEST(IntelMappingValidatorTest, InvalidMapping_Rotate90) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kDefaultDstRight, kDefaultDstBottom},
                /*rotate90=*/true);

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test checks that a source crop with an odd X coordinate is rejected,
// as the hardware requires even alignment when scaling is involved.
TEST(IntelMappingValidatorTest, InvalidMapping_OddSourceX) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{kOddSrcLeft, 0.0F, kOddSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test verifies that a source crop with an odd width is rejected when
// scaling is required.
TEST(IntelMappingValidatorTest, InvalidMapping_OddSourceWidth) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kOddWidthSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test ensures that a mapping is rejected if the source width exceeds
// the hardware scaler limit of 4096.
TEST(IntelMappingValidatorTest, InvalidMapping_MaxSrcWidthExceeded) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/
                {0.0F, 0.0F, kMaxSrcWidthExceededRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test ensures that a mapping is rejected if the source height exceeds
// the hardware scaler limit of 8192.
TEST(IntelMappingValidatorTest, InvalidMapping_MaxSrcHeightExceeded) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/
                {0.0F, 0.0F, kDefaultSrcRight, kMaxSrcHeightExceededBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test checks that a mapping is rejected if the destination width exceeds
// the hardware scaler limit of 8192.
TEST(IntelMappingValidatorTest, InvalidMapping_MaxDstWidthExceeded) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kMaxDstWidthExceededRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test checks that a mapping is rejected if the destination height exceeds
// the hardware scaler limit of 8192.
TEST(IntelMappingValidatorTest, InvalidMapping_MaxDstHeightExceeded) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kMaxDstHeightExceededBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

// This test verifies that a mapping using exactly the maximum allowed number
// of hardware scalers (2) is accepted.
TEST(IntelMappingValidatorTest, ValidMapping_MaxScalersCount) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  HwcLayer layer2(&mock_display);
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});
  SetLayerProps(/*layer=*/layer2,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice},
                                        {&layer2, CompositionType::kDevice}};
  EXPECT_TRUE(IntelValidateMapping(mappings));
}

// This test ensures that a mapping requiring more hardware scalers than
// available (limit is 2) is rejected.
TEST(IntelMappingValidatorTest, InvalidMapping_TooManyScalers) {
  MockCompositorDisplay mock_display;
  HwcLayer layer1(&mock_display);
  HwcLayer layer2(&mock_display);
  HwcLayer layer3(&mock_display);
  // 3 scaled layers push scalers_used to 3, exceeding the hardware limit of 2
  SetLayerProps(/*layer=*/layer1,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});
  SetLayerProps(/*layer=*/layer2,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});
  SetLayerProps(/*layer=*/layer3,
                /*src=*/{0.0F, 0.0F, kDefaultSrcRight, kDefaultSrcBottom},
                /*dst=*/{0, 0, kScaledDstRight, kScaledDstBottom});

  std::vector<LayerMapping> mappings = {{&layer1, CompositionType::kDevice},
                                        {&layer2, CompositionType::kDevice},
                                        {&layer3, CompositionType::kDevice}};
  EXPECT_FALSE(IntelValidateMapping(mappings));
}

}  // namespace
}  // namespace android::drm_hwcomposer