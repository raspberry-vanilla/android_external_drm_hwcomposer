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

#include <drm/drm_mode.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "utils/ColorUtil.h"
#include "utils/TestUtils.h"

namespace android::drm_hwcomposer {

// Tests for EvaluateHlgOetf
TEST(ColorUtilTest, EvaluateHlgOetfNegativeAndZero) {
  EXPECT_DOUBLE_EQ(ColorUtil::EvaluateHlgOetf(-0.5), 0.0);
  EXPECT_DOUBLE_EQ(ColorUtil::EvaluateHlgOetf(0.0), 0.0);
}

TEST(ColorUtilTest, EvaluateHlgOetfLinearRegion) {
  // For 0 < l <= 1/12, HlgOetf(l) = 0.5 * sqrt(12 * l)
  double l_val = 0.5 / 12.0;
  double expected = 0.5 * std::sqrt(0.5);
  EXPECT_NEAR(ColorUtil::EvaluateHlgOetf(l_val), expected, 1e-6);

  // At upper linear boundary: l = 1/12 -> 0.5 * sqrt(1) = 0.5
  EXPECT_NEAR(ColorUtil::EvaluateHlgOetf(1.0 / 12.0), 0.5, 1e-6);
}

TEST(ColorUtilTest, EvaluateHlgOetfLogarithmicRegion) {
  // For l > 1/12, HlgOetf(l) = a * ln(12*l - b) + c
  // At l = 1.0 (reference white level of 100 cd/m2), result should be 1.0
  EXPECT_NEAR(ColorUtil::EvaluateHlgOetf(1.0), 1.0, 1e-5);
}

// Tests for ToColorTransform
TEST(ColorUtilTest, ToColorTransform3x3Nullptr) {
  std::shared_ptr<const HalColorTransformMatrix> null_matrix = nullptr;
  EXPECT_EQ(ColorUtil::ToColorTransform3x3(null_matrix), nullptr);
}

TEST(ColorUtilTest, ToColorTransform3x3Identity) {
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(
      kIdentityMatrix);
  auto ctm = ColorUtil::ToColorTransform3x3(hal_matrix);
  ASSERT_NE(ctm, nullptr);

  constexpr uint64_t kOneFixPt = 1ULL << 32;

  // Row-major 3x3 s31.32 fixed point matrix
  // [1.0, 0.0, 0.0]
  // [0.0, 1.0, 0.0]
  // [0.0, 0.0, 1.0]
  EXPECT_EQ(ctm->matrix[0], kOneFixPt);
  EXPECT_EQ(ctm->matrix[1], 0ULL);
  EXPECT_EQ(ctm->matrix[2], 0ULL);

  EXPECT_EQ(ctm->matrix[3], 0ULL);
  EXPECT_EQ(ctm->matrix[4], kOneFixPt);
  EXPECT_EQ(ctm->matrix[5], 0ULL);

  EXPECT_EQ(ctm->matrix[6], 0ULL);
  EXPECT_EQ(ctm->matrix[7], 0ULL);
  EXPECT_EQ(ctm->matrix[8], kOneFixPt);
}

TEST(ColorUtilTest, ToColorTransform3x3ValuesAndSigns) {
  // Transposed 4x4 matrix from HAL:
  // [ 0.5   0.0   0.0   0.0 ]
  // [ 0.25  0.75  0.0   0.0 ]
  // [ 0.0   0.0  -0.5   0.0 ]
  // [ 0.0   0.0   0.0   1.0 ]
  // HAL is transposed, so R_out = 0.5*R + 0.25*G, G_out = 0.75*G, B_out =
  // -0.5*B
  HalColorTransformMatrix matrix = {
      0.5F,  0.0F,  0.0F,  0.0F,  //
      0.25F, 0.75F, 0.0F,  0.0F,  //
      0.0F,  0.0F,  -0.5F, 0.0F,  //
      0.0F,  0.0F,  0.0F,  1.0F,  //
  };
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(matrix);
  auto ctm = ColorUtil::ToColorTransform3x3(hal_matrix);
  ASSERT_NE(ctm, nullptr);

  constexpr uint64_t kSignMask = 1ULL << 63;
  // NOLINTBEGIN(readability-identifier-naming)
  constexpr auto kVal0_5 = static_cast<uint64_t>(0.5 * (1ULL << 32));
  constexpr auto kVal0_25 = static_cast<uint64_t>(0.25 * (1ULL << 32));
  constexpr auto kVal0_75 = static_cast<uint64_t>(0.75 * (1ULL << 32));
  // NOLINTEND(readability-identifier-naming)

  EXPECT_EQ(ctm->matrix[0], kVal0_5);
  EXPECT_EQ(ctm->matrix[1], kVal0_25);
  EXPECT_EQ(ctm->matrix[2], 0ULL);

  EXPECT_EQ(ctm->matrix[3], 0ULL);
  EXPECT_EQ(ctm->matrix[4], kVal0_75);
  EXPECT_EQ(ctm->matrix[5], 0ULL);

  EXPECT_EQ(ctm->matrix[6], 0ULL);
  EXPECT_EQ(ctm->matrix[7], 0ULL);
  EXPECT_EQ(ctm->matrix[8], kVal0_5 | kSignMask);
}

TEST(ColorUtilTest, ToColorTransform3x4Nullptr) {
  std::shared_ptr<const HalColorTransformMatrix> null_matrix = nullptr;
  EXPECT_EQ(ColorUtil::ToColorTransform3x4(null_matrix), nullptr);
}

TEST(ColorUtilTest, ToColorTransform3x4Identity) {
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(
      kIdentityMatrix);
  auto ctm = ColorUtil::ToColorTransform3x4(hal_matrix);
  ASSERT_NE(ctm, nullptr);

  constexpr uint64_t kOneFixPt = 1ULL << 32;

  // 3x4 s31.32 fixed point matrix
  // [1.0, 0.0, 0.0, 0.0]
  // [0.0, 1.0, 0.0, 0.0]
  // [0.0, 0.0, 1.0, 0.0]
  EXPECT_EQ(ctm->matrix[0], kOneFixPt);
  EXPECT_EQ(ctm->matrix[1], 0ULL);
  EXPECT_EQ(ctm->matrix[2], 0ULL);
  EXPECT_EQ(ctm->matrix[3], 0ULL);

  EXPECT_EQ(ctm->matrix[4], 0ULL);
  EXPECT_EQ(ctm->matrix[5], kOneFixPt);
  EXPECT_EQ(ctm->matrix[6], 0ULL);
  EXPECT_EQ(ctm->matrix[7], 0ULL);

  EXPECT_EQ(ctm->matrix[8], 0ULL);
  EXPECT_EQ(ctm->matrix[9], 0ULL);
  EXPECT_EQ(ctm->matrix[10], kOneFixPt);
  EXPECT_EQ(ctm->matrix[11], 0ULL);
}

TEST(ColorUtilTest, ToColorTransform3x4ValuesAndSigns) {
  HalColorTransformMatrix matrix = {
      0.5F,   0.0F,  0.0F,  0.0F,  //
      0.25F,  0.75F, 0.0F,  0.0F,  //
      0.0F,   0.0F,  -0.5F, 0.0F,  //
      0.125F, 0.0F,  0.0F,  1.0F,  //
  };
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(matrix);
  auto ctm = ColorUtil::ToColorTransform3x4(hal_matrix);
  ASSERT_NE(ctm, nullptr);

  constexpr uint64_t kSignMask = 1ULL << 63;
  // NOLINTBEGIN(readability-identifier-naming)
  constexpr auto kVal0_5 = static_cast<uint64_t>(0.5 * (1ULL << 32));
  constexpr auto kVal0_25 = static_cast<uint64_t>(0.25 * (1ULL << 32));
  constexpr auto kVal0_75 = static_cast<uint64_t>(0.75 * (1ULL << 32));
  constexpr auto kVal0_125 = static_cast<uint64_t>(0.125 * (1ULL << 32));
  // NOLINTEND(readability-identifier-naming)

  EXPECT_EQ(ctm->matrix[0], kVal0_5);
  EXPECT_EQ(ctm->matrix[1], kVal0_25);
  EXPECT_EQ(ctm->matrix[2], 0ULL);
  EXPECT_EQ(ctm->matrix[3], kVal0_125);

  EXPECT_EQ(ctm->matrix[4], 0ULL);
  EXPECT_EQ(ctm->matrix[5], kVal0_75);
  EXPECT_EQ(ctm->matrix[6], 0ULL);
  EXPECT_EQ(ctm->matrix[7], 0ULL);

  EXPECT_EQ(ctm->matrix[8], 0ULL);
  EXPECT_EQ(ctm->matrix[9], 0ULL);
  EXPECT_EQ(ctm->matrix[10], kVal0_5 | kSignMask);
  EXPECT_EQ(ctm->matrix[11], 0ULL);
}

// Tests for GamutAdjustIfNeeded
TEST(ColorUtilTest, GamutAdjustIfNeededSameColorspace) {
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(
      kIdentityMatrix);
  CscCache cache;

  auto ctm3x3 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm>(HwcColorspace::kBt709, HwcColorspace::kBt709, hal_matrix,
                     cache);
  ASSERT_NE(ctm3x3, nullptr);
  EXPECT_TRUE(cache.empty());

  auto ctm3x4 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm_3x4>(HwcColorspace::kBt709, HwcColorspace::kBt709,
                         hal_matrix, cache);
  ASSERT_NE(ctm3x4, nullptr);
  EXPECT_TRUE(cache.empty());
}

TEST(ColorUtilTest, GamutAdjustIfNeededDifferentColorspaceAndCaching) {
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(
      kIdentityMatrix);
  CscCache cache;

  auto ctm3x3 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm>(HwcColorspace::kBt709, HwcColorspace::kDciP3, hal_matrix,
                     cache);
  ASSERT_NE(ctm3x3, nullptr);
  EXPECT_EQ(cache.size(), 1U);

  // Second call with same parameters should reuse cache entry
  auto ctm3x3_cached = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm>(HwcColorspace::kBt709, HwcColorspace::kDciP3, hal_matrix,
                     cache);
  ASSERT_NE(ctm3x3_cached, nullptr);
  EXPECT_EQ(cache.size(), 1U);

  // Test 3x4 overload with different colorspaces
  auto ctm3x4 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm_3x4>(HwcColorspace::kBt709, HwcColorspace::kBt2020,
                         hal_matrix, cache);
  ASSERT_NE(ctm3x4, nullptr);
  EXPECT_EQ(cache.size(), 2U);
}

// Tests for GetDegammaLut
TEST(ColorUtilTest, GetDegammaLutInvalidLutSize) {
  Lut1DCache<drm_color_lut32> cache;

  // Invalid size < 2
  const auto &lut0 = ColorUtil::GetDegammaLut(TransferFunction::kPq, 0, cache,
                                              1.0F);
  EXPECT_TRUE(lut0.empty());

  const auto &lut1 = ColorUtil::GetDegammaLut(TransferFunction::kPq, 1, cache,
                                              1.0F);
  EXPECT_TRUE(lut1.empty());
}

TEST(ColorUtilTest, GetDegammaLutValidAndCaching) {
  Lut1DCache<drm_color_lut32> cache;

  static constexpr size_t kLutSize = 256;
  const auto &lut = ColorUtil::GetDegammaLut(TransferFunction::kPq, kLutSize,
                                             cache, 1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_EQ(cache.size(), 1U);

  // Check initial and final values (linear / EOTF mapping)
  EXPECT_EQ(lut[0].red, 0U);
  EXPECT_EQ(lut[0].green, 0U);
  EXPECT_EQ(lut[0].blue, 0U);

  EXPECT_GT(lut[kLutSize - 1].red, 0U);

  // Second call with identical arguments should return exact cached reference
  const auto &lut_cached = ColorUtil::GetDegammaLut(TransferFunction::kPq,
                                                    kLutSize, cache, 1.0F);
  EXPECT_EQ(&lut, &lut_cached);
}

// Tests for GetGammaLut
TEST(ColorUtilTest, GetGammaLutInvalidLutSize) {
  Lut1DCache<drm_color_lut> cache;

  const auto &lut = ColorUtil::GetGammaLut(TransferFunction::kHlg, 1, cache,
                                           1.0F, 1.0F);
  EXPECT_TRUE(lut.empty());
}

TEST(ColorUtilTest, GetGammaLutValidAndCaching) {
  Lut1DCache<drm_color_lut> cache;

  static constexpr size_t kLutSize = 512;
  const auto &lut = ColorUtil::GetGammaLut(TransferFunction::kHlg, kLutSize,
                                           cache, 1.0F, 1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_EQ(cache.size(), 1U);

  EXPECT_EQ(lut[0].red, 0U);
  EXPECT_EQ(lut[0].green, 0U);
  EXPECT_EQ(lut[0].blue, 0U);

  EXPECT_GT(lut[kLutSize - 1].red, 0U);

  // Second call with identical arguments should return exact cached reference
  const auto &lut_cached = ColorUtil::GetGammaLut(TransferFunction::kHlg,
                                                  kLutSize, cache, 1.0F, 1.0F);
  EXPECT_EQ(&lut, &lut_cached);
}

TEST(ColorUtilTest, GetGammaLutHdrClampsToMinFloorAtZeroDisplayBrightness) {
  Lut1DCache<drm_color_lut> cache;
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.01");

  static constexpr size_t kLutSize = 512;
  const auto &lut = ColorUtil::GetGammaLut(TransferFunction::kHlg, kLutSize,
                                           cache,
                                           ColorUtil::ScaleBrightnessIfNeeded(
                                               0.0F),
                                           1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_GT(lut[kLutSize - 1].red, 0U);
  EXPECT_GT(lut[kLutSize - 1].green, 0U);
  EXPECT_GT(lut[kLutSize - 1].blue, 0U);
}

// Tests for ScaleBrightnessIfNeeded
TEST(ColorUtilTest, ScaleBrightnessIfNeededDefaultZero) {
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(-1.0F), -1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.0F), 0.0F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.5F), 0.5F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(1.0F), 1.0F);
}

TEST(ColorUtilTest, ScaleBrightnessIfNeededClampsToMinFloor) {
  ScopedTestProperty prop("vendor.hwc.drm.min_display_brightness", "0.01");

  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(-1.0F), -1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.0F), 0.01F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.005F), 0.01F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.5F), 0.5F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(1.0F), 1.0F);
}

TEST(ColorUtilTest, ScaleBrightnessIfNeededInvalidStringDefaultsToZero) {
  ScopedTestProperty prop("vendor.hwc.drm.min_display_brightness", "invalid");

  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.0F), 0.0F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.5F), 0.5F);
}

TEST(ColorUtilTest, ScaleBrightnessIfNeededOutOfRangeDefaultsToZero) {
  ScopedTestProperty prop("vendor.hwc.drm.min_display_brightness", "1.5");

  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.0F), 0.0F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.5F), 0.5F);
}

TEST(ColorUtilTest, ScaleBrightnessIfNeededRangeScaleNoopWhenMinZero) {
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.0F), 0.0F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.5F), 0.5F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(1.0F), 1.0F);
}

TEST(ColorUtilTest, ScaleBrightnessIfNeededRangeScaleNonZeroAtZero) {
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.0F), 0.1F);
}

TEST(ColorUtilTest, ScaleBrightnessIfNeededRangeScaleCalculatesExpectedScale) {
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  // expected scale = 0.1 + 0.5 * (1.0 - 0.1) = 0.55
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(0.5F), 0.55F);
  EXPECT_FLOAT_EQ(ColorUtil::ScaleBrightnessIfNeeded(1.0F), 1.0F);
}

TEST(ColorUtilTest, GetGammaLutHdrScaleRangeNoopWhenMinZero) {
  Lut1DCache<drm_color_lut> cache;
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  static constexpr size_t kLutSize = 512;
  const auto &lut = ColorUtil::GetGammaLut(TransferFunction::kHlg, kLutSize,
                                           cache,
                                           ColorUtil::ScaleBrightnessIfNeeded(
                                               0.0F),
                                           1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_EQ(lut[kLutSize - 1].red, 0U);
  EXPECT_EQ(lut[kLutSize - 1].green, 0U);
  EXPECT_EQ(lut[kLutSize - 1].blue, 0U);
}

TEST(ColorUtilTest, GetGammaLutHdrScaleRangeNonZeroAtZero) {
  Lut1DCache<drm_color_lut> cache;
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  static constexpr size_t kLutSize = 512;
  const auto &lut = ColorUtil::GetGammaLut(TransferFunction::kHlg, kLutSize,
                                           cache,
                                           ColorUtil::ScaleBrightnessIfNeeded(
                                               0.0F),
                                           1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_GT(lut[kLutSize - 1].red, 0U);
  EXPECT_GT(lut[kLutSize - 1].green, 0U);
  EXPECT_GT(lut[kLutSize - 1].blue, 0U);
}

TEST(ColorUtilTest, GetGammaLutHdrScaleRangeCalculatesExpectedScale) {
  Lut1DCache<drm_color_lut> cache;
  static constexpr size_t kLutSize = 512;

  const auto &lut_scaled = [&]() -> const auto & {
    ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
    ScopedTestProperty
        scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                   "true");
    return ColorUtil::GetGammaLut(TransferFunction::kHlg, kLutSize, cache,
                                  ColorUtil::ScaleBrightnessIfNeeded(0.5F),
                                  1.0F);
  }();

  const auto &lut_expected = ColorUtil::GetGammaLut(TransferFunction::kHlg,
                                                    kLutSize, cache, 0.55F,
                                                    1.0F);

  EXPECT_EQ(&lut_scaled, &lut_expected);
}

}  // namespace android::drm_hwcomposer

// NOLINTEND(readability-magic-numbers)