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

#include <array>
#include <memory>
#include <random>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "compositor/CompositionPlanner.h"
#include "compositor/CompositorTestUtils.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/ShortCircuitor.h"
#include "drm/DrmTestUtils.h"

namespace android::drm_hwcomposer {

namespace {

using ::testing::_;
using ::testing::AtMost;
using ::testing::Return;
using ::testing::ReturnRef;

constexpr IRect kLayerRect = {.left = 0,
                              .top = 0,
                              .right = 1920,
                              .bottom = 1080};

constexpr ShortCircuitor::Config kDefaultConfig = {.enabled = true,
                                                   .ignore_geometry = false,
                                                   .ignore_ctm = false};

using LayerList = std::vector<HwcLayer>;
using DrmPlanePtr = std::shared_ptr<FakeDrmPlane>;

auto CreateBaselineLayers(MockCompositorDisplay& display) -> LayerList {
  return {CompositorTestUtils::CreateLayer(&display, kLayerRect, /*z_order=*/0,
                                           CompositionType::kDevice),
          CompositorTestUtils::CreateLayer(&display, kLayerRect, /*z_order=*/1,
                                           CompositionType::kClient),
          CompositorTestUtils::CreateLayer(&display, kLayerRect, /*z_order=*/2,
                                           CompositionType::kClient),
          CompositorTestUtils::CreateLayer(&display, kLayerRect, /*z_order=*/3,
                                           CompositionType::kCursor)};
}

auto ToConstPtrList(const LayerList& layers) -> std::vector<const HwcLayer*> {
  std::vector<const HwcLayer*> result;
  result.reserve(layers.size());
  for (const auto& layer : layers)
    result.emplace_back(&layer);
  return result;
}

auto CreateRequestContext(const ICompositorDisplay& display)
    -> ValidationRequestContext {
  return {display, display.GetOrderLayersByZPos()};
}

auto CreateCompositionTypeMap(const std::vector<const HwcLayer*>& layers)
    -> CompositionPlanner::CompositionTypeMap {
  CompositionPlanner::CompositionTypeMap type_map;
  for (const auto* layer : layers)
    type_map.emplace(layer, layer->GetSfType());
  return type_map;
}

auto CreateValidatedComposition(const std::vector<const HwcLayer*>& layers)
    -> CompositionPlanner::ValidatedComposition {
  return {.composition_types = CreateCompositionTypeMap(layers),
          .flatten_reason = CompositionPlanner::FlattenReason::kNone,
          .cursor_plane_validated = layers.back()->GetSfType() ==
                                    CompositionType::kCursor};
}

}  // namespace

class ShortCircuitorTest : public ::testing::Test {
 protected:
  enum class Expectation { FullValidation, ShortCircuited };

  using ReqCtxOverrideFunc = std::function<
      ValidationRequestContext(MockCompositorDisplay&, LayerList&)>;
  using CompositionOverrideFunc = std::function<
      CompositionPlanner::ValidatedComposition(
          CompositionPlanner::ValidatedComposition&&)>;
  using ConfigOverrideFunc = std::function<ShortCircuitor::Config()>;

  void SetLastRequestOverride(ReqCtxOverrideFunc func) {
    last_req_override_func_ = func;
  }
  void SetCurrentRequestOverride(ReqCtxOverrideFunc func) {
    curr_req_override_func_ = func;
  }
  void SetLastCompositionOverride(CompositionOverrideFunc func) {
    last_composition_override_func_ = func;
  }

  static void SetUpDisplay(MockCompositorDisplay& display) {
    ON_CALL(display, GetColorTransformMatrix())
        .WillByDefault(Return(
            std::make_shared<const HalColorTransformMatrix>(kIdentityMatrix)));
  }
  static void SetUpCursorPlane(MockCompositorDisplay& display,
                               FakeDrmDevice& drm_device,
                               DrmPlanePtr& cursor_plane) {
    cursor_plane = std::make_shared<FakeDrmPlane>(drm_device,
                                                  DRM_PLANE_TYPE_CURSOR);
    cursor_plane->is_valid_ = true;

    // Expected to be called from ShortCircuitor::Check()
    EXPECT_CALL(display, GetCursorPlane())
        .Times(AtMost(1))
        .WillRepeatedly(Return(cursor_plane->BindPipeline(nullptr)));
    EXPECT_CALL(display, CursorPlaneNeedsColorPipeline(_))
        .Times(AtMost(1))
        .WillRepeatedly(Return(false));
  }
  void Test(ShortCircuitor::Config config, Expectation expectation) const {
    // Display
    MockCompositorDisplay display;
    ASSERT_NO_FATAL_FAILURE(SetUpDisplay(display));

    // Cursor plane
    FakeDrmDevice drm_device;
    DrmPlanePtr cursor_plane;
    ASSERT_NO_FATAL_FAILURE(
        SetUpCursorPlane(display, drm_device, cursor_plane));

    // Last presented layers
    auto layers = CreateBaselineLayers(display);
    auto layers_ptr_list = ToConstPtrList(layers);
    EXPECT_CALL(display, GetOrderLayersByZPos())
        .WillRepeatedly(Return(layers_ptr_list));

    // Prepare last presented composition cache
    PresentedCompositionCache composition_cache;
    EXPECT_CALL(display, GetLastPresentedComposition())
        .Times(AtMost(1))  // Called from ShortCircuitor::Check()
        .WillRepeatedly(ReturnRef(composition_cache));

    // Last request context & validated composition
    ValidationRequestContext
        last_request = last_req_override_func_
                           ? last_req_override_func_(display, layers)
                           : ValidationRequestContext(display, layers_ptr_list);

    auto composition = CreateValidatedComposition(layers_ptr_list);
    if (last_composition_override_func_)
      composition = last_composition_override_func_(std::move(composition));

    ASSERT_TRUE(last_request);
    ASSERT_TRUE(composition_cache.SetRequestedContext(last_request));
    ASSERT_TRUE(composition_cache.SetValidatedComposition(composition));
    ASSERT_TRUE(composition_cache.GetContext().has_value());

    // Test ShortCircuitor::Get()
    const auto short_circuited = ShortCircuitor::
        Get(config, composition_cache,
            curr_req_override_func_ ? curr_req_override_func_(display, layers)
                                    : last_request);
    if (expectation == Expectation::ShortCircuited) {
      EXPECT_TRUE(short_circuited.has_value());
      EXPECT_EQ(*short_circuited, composition);
    } else {
      EXPECT_FALSE(short_circuited.has_value());
      EXPECT_EQ(short_circuited, std::nullopt);
    }
  }

  ReqCtxOverrideFunc last_req_override_func_;
  ReqCtxOverrideFunc curr_req_override_func_;
  CompositionOverrideFunc last_composition_override_func_;
};

// ---------------------------------------------------------------------------
// Testing core behaviors of ShortCircuitor::Get()

TEST_F(ShortCircuitorTest, SuccessfullyShortCircuited) {
  // No overrides, expect successful short circuiting.
  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::ShortCircuited));
}

TEST_F(ShortCircuitorTest, PreviouslyFlattened) {
  // Override the last presented composition to be flattened.
  SetLastCompositionOverride(
      [](CompositionPlanner::ValidatedComposition&& composition)
          -> CompositionPlanner::ValidatedComposition {
        auto flattened_composition = std::move(composition);
        flattened_composition
            .flatten_reason = CompositionPlanner::FlattenReason::kStaticScene;
        return flattened_composition;
      });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, DifferentDisplay) {
  // Override the current request to have a different display.
  MockCompositorDisplay other_display;
  SetCurrentRequestOverride(
      [&other_display](MockCompositorDisplay& display,
                       LayerList& layers) -> ValidationRequestContext {
        // Copy over display properties from the original display.
        EXPECT_CALL(other_display, GetOrderLayersByZPos())
            .WillRepeatedly(Return(display.GetOrderLayersByZPos()));
        EXPECT_CALL(other_display, GetColorTransformMatrix())
            .WillRepeatedly(Return(display.GetColorTransformMatrix()));

        return ValidationRequestContext(other_display, ToConstPtrList(layers));
      });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, DifferentColorMatrix) {
  // Override the current request to have a different color matrix.
  SetCurrentRequestOverride([](MockCompositorDisplay& display,
                               LayerList& layers) -> ValidationRequestContext {
    // Assume display had identity color matrix during last request.
    EXPECT_CALL(display, GetColorTransformMatrix())
        .WillOnce(Return(std::make_shared<const HalColorTransformMatrix>()));

    return ValidationRequestContext(display, ToConstPtrList(layers));
  });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, OneLayerAdded) {
  // Override the last request to remove the last layer pointer.
  SetLastRequestOverride([](MockCompositorDisplay& display,
                            LayerList& layers) -> ValidationRequestContext {
    auto layer_ptrs = ToConstPtrList(layers);
    layer_ptrs.pop_back();
    return ValidationRequestContext(display, layer_ptrs);
  });

  // Should still short circuit if only overriding the last request.
  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::ShortCircuited));

  // Override the current request to be the exact layers before layer removal.
  SetCurrentRequestOverride([](MockCompositorDisplay& display,
                               LayerList& layers) -> ValidationRequestContext {
    return ValidationRequestContext(display, ToConstPtrList(layers));
  });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, OneLayerRemoved) {
  // Override the current request to remove the last layer.
  SetCurrentRequestOverride([](MockCompositorDisplay& display,
                               LayerList& layers) -> ValidationRequestContext {
    auto layer_ptrs = ToConstPtrList(layers);
    layer_ptrs.pop_back();
    return ValidationRequestContext(display, layer_ptrs);
  });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, DifferentCompositionTypes) {
  // Override the current request to have a different composition type.
  SetCurrentRequestOverride([](MockCompositorDisplay& display,
                               LayerList& layers) -> ValidationRequestContext {
    // Flip the last layer's composition type.
    const auto new_type = layers.back().GetSfType() == CompositionType::kCursor
                              ? CompositionType::kClient
                              : CompositionType::kCursor;
    layers.back().SetLayerProperties({.composition_type = new_type});
    return ValidationRequestContext(display, ToConstPtrList(layers));
  });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, DifferentSourceRects) {
  // Generate a random FRect as source rect.
  std::mt19937 gen(std::random_device{}());
  std::uniform_real_distribution<float> dist(0.F, 720.F);
  const auto src_rect = SrcRectInfo{.f_rect = FRect{.left = 0.F,
                                                    .top = 0.F,
                                                    .right = dist(gen),
                                                    .bottom = dist(gen)}};

  // Override the current request to have a different source rect.
  SetCurrentRequestOverride(
      [&src_rect](MockCompositorDisplay& display,
                  LayerList& layers) -> ValidationRequestContext {
        // Change the first layer's source rect.
        layers.front().SetLayerProperties({.source_crop = src_rect});
        return ValidationRequestContext(display, ToConstPtrList(layers));
      });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, DifferentDisplayRects) {
  // Generate a random IRect as display rect.
  std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 1080);
  const auto disp_rect = DstRectInfo{.i_rect = IRect{.left = 0,
                                                     .top = 0,
                                                     .right = dist(gen),
                                                     .bottom = dist(gen)}};

  // Override the current request to have a different display rect.
  SetCurrentRequestOverride(
      [&disp_rect](MockCompositorDisplay& display,
                   LayerList& layers) -> ValidationRequestContext {
        // Change the first layer's display rect.
        layers.front().SetLayerProperties({.display_frame = disp_rect});
        return ValidationRequestContext(display, ToConstPtrList(layers));
      });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

TEST_F(ShortCircuitorTest, DifferentAlphas) {
  // Override the current request to have a different alpha.
  SetCurrentRequestOverride([](MockCompositorDisplay& display,
                               LayerList& layers) -> ValidationRequestContext {
    // Flip the first layer's alpha, as the last layer might be the cursor.
    const auto new_alpha = layers.front().GetLayerData().pi.alpha == 1.F ? 0.5F
                                                                         : 1.F;
    layers.front().SetLayerProperties({.alpha = new_alpha});
    return ValidationRequestContext(display, ToConstPtrList(layers));
  });

  ASSERT_NO_FATAL_FAILURE(Test(kDefaultConfig, Expectation::FullValidation));
}

// ---------------------------------------------------------------------------
// Testing ShortCircuitor::Config

TEST_F(ShortCircuitorTest, Disabled) {
  ShortCircuitor::Config config = kDefaultConfig;
  config.enabled = false;
  ASSERT_NO_FATAL_FAILURE(Test(config, Expectation::FullValidation));
}

// See also ShortCircuitorTest#DifferentSourceRects and #DifferentDisplayRects
TEST_F(ShortCircuitorTest, IgnoreGeometries) {
  // Generate random rects.
  std::mt19937 gen(std::random_device{}());
  std::uniform_real_distribution<float> f_dist(0.F, 720.F);
  const auto src_rect = SrcRectInfo{.f_rect = FRect{.left = 0.F,
                                                    .top = 0.F,
                                                    .right = f_dist(gen),
                                                    .bottom = f_dist(gen)}};
  std::uniform_int_distribution<int> i_dist(0, 1080);
  const auto disp_rect = DstRectInfo{.i_rect = IRect{.left = 0,
                                                     .top = 0,
                                                     .right = i_dist(gen),
                                                     .bottom = i_dist(gen)}};

  // Override the current request to have different source and display rects.
  SetCurrentRequestOverride(
      [&src_rect, &disp_rect](MockCompositorDisplay& display,
                              LayerList& layers) -> ValidationRequestContext {
        // Change the first layer's geometries.
        layers.front().SetLayerProperties(
            {.source_crop = src_rect, .display_frame = disp_rect});
        return ValidationRequestContext(display, ToConstPtrList(layers));
      });

  ShortCircuitor::Config config = kDefaultConfig;
  config.ignore_geometry = true;
  ASSERT_NO_FATAL_FAILURE(Test(config, Expectation::ShortCircuited));
}

// See also ShortCircuitorTest#DifferentColorMatrix
TEST_F(ShortCircuitorTest, IgnoreColorMatrix) {
  // Override the current request to have a different color matrix.
  SetCurrentRequestOverride([](MockCompositorDisplay& display,
                               LayerList& layers) -> ValidationRequestContext {
    // Assume display had identity color matrix during last request.
    EXPECT_CALL(display, GetColorTransformMatrix())
        .WillOnce(Return(std::make_shared<const HalColorTransformMatrix>()));

    return ValidationRequestContext(display, ToConstPtrList(layers));
  });

  ShortCircuitor::Config config = kDefaultConfig;
  config.ignore_ctm = true;
  ASSERT_NO_FATAL_FAILURE(Test(config, Expectation::ShortCircuited));
}

}  // namespace android::drm_hwcomposer
