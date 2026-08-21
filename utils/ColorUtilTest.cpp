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
#include <math/mat3.h>
#include <math/vec3.h>
#include <ui/ColorSpace.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/DrmColorspace.h"
#include "utils/ColorUtil.h"
#include "utils/TestUtils.h"

using ColorGamut = android::ColorSpace;

namespace android::drm_hwcomposer {

namespace {

inline double From3132FixPt(uint64_t val) {
  constexpr uint64_t kSignMask = 1ULL << 63;
  double res = static_cast<double>(val & ~kSignMask) /
               static_cast<double>(1ULL << 32);
  return (val & kSignMask) != 0U ? -res : res;
}

}  // namespace
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

  auto ctm3x3 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm>(HwcColorspace::kBt709, HwcColorspace::kBt709, hal_matrix);
  ASSERT_NE(ctm3x3, nullptr);

  auto ctm3x4 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm_3x4>(HwcColorspace::kBt709, HwcColorspace::kBt709,
                         hal_matrix);
  ASSERT_NE(ctm3x4, nullptr);
}

TEST(ColorUtilTest, GamutAdjustIfNeededDifferentColorspace) {
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(
      kIdentityMatrix);

  auto ctm3x3 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm>(HwcColorspace::kBt709, HwcColorspace::kDciP3, hal_matrix);
  ASSERT_NE(ctm3x3, nullptr);

  // Test 3x4 overload with different colorspaces
  auto ctm3x4 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm_3x4>(HwcColorspace::kBt709, HwcColorspace::kBt2020,
                         hal_matrix);
  ASSERT_NE(ctm3x4, nullptr);
}

TEST(ColorUtilTest, GamutAdjust3x4OffsetVectorRotation) {
  HalColorTransformMatrix matrix = {
      1.0F, 0.0F, 0.0F, 0.0F,  //
      0.0F, 1.0F, 0.0F, 0.0F,  //
      0.0F, 0.0F, 1.0F, 0.0F,  //
      0.1F, 0.2F, 0.3F, 1.0F,  // Offsets O_src = [0.1, 0.2, 0.3]^T
  };
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(matrix);

  auto ctm3x4 = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm_3x4>(HwcColorspace::kBt709, HwcColorspace::kBt2020,
                         hal_matrix);
  ASSERT_NE(ctm3x4, nullptr);

  mat3d gamut_transform(
      ColorSpaceConnector(ColorGamut::BT709(), ColorGamut::BT2020())
          .getTransform());
  double3 expected_o = gamut_transform * double3(0.1, 0.2, 0.3);

  double actual_o_r = From3132FixPt(ctm3x4->matrix[3]);
  double actual_o_g = From3132FixPt(ctm3x4->matrix[7]);
  double actual_o_b = From3132FixPt(ctm3x4->matrix[11]);

  EXPECT_NEAR(actual_o_r, expected_o[0], 1e-4);
  EXPECT_NEAR(actual_o_g, expected_o[1], 1e-4);
  EXPECT_NEAR(actual_o_b, expected_o[2], 1e-4);
}

TEST(ColorUtilTest, HasOffsetDetection) {
  EXPECT_FALSE(ColorUtil::HasOffset(kIdentityMatrix));

  HalColorTransformMatrix offset_r = kIdentityMatrix;
  offset_r[12] = 0.05F;
  EXPECT_TRUE(ColorUtil::HasOffset(offset_r));

  HalColorTransformMatrix offset_g = kIdentityMatrix;
  offset_g[13] = -0.05F;
  EXPECT_TRUE(ColorUtil::HasOffset(offset_g));

  HalColorTransformMatrix offset_b = kIdentityMatrix;
  offset_b[14] = 0.1F;
  EXPECT_TRUE(ColorUtil::HasOffset(offset_b));

  // Sub-epsilon offset (< std::numeric_limits<float>::epsilon()) is ignored as
  // noise
  HalColorTransformMatrix matrix_sub_eps = kIdentityMatrix;
  matrix_sub_eps[12] = 1e-8F;
  EXPECT_FALSE(ColorUtil::HasOffset(matrix_sub_eps));
}

TEST(ColorUtilTest, ToColorOffsetNullptrAndBasic) {
  EXPECT_EQ(ColorUtil::ToColorOffset(nullptr), nullptr);

  HalColorTransformMatrix matrix = {
      1.0F,  0.0F, 0.0F,  0.0F,  //
      0.0F,  1.0F, 0.0F,  0.0F,  //
      0.0F,  0.0F, 1.0F,  0.0F,  //
      0.25F, 0.5F, 0.75F, 1.0F,  //
  };
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(matrix);
  auto offsets = ColorUtil::ToColorOffset(hal_matrix);
  ASSERT_NE(offsets, nullptr);

  EXPECT_NEAR(From3132FixPt((*offsets)[0]), 0.25, 1e-4);
  EXPECT_NEAR(From3132FixPt((*offsets)[1]), 0.5, 1e-4);
  EXPECT_NEAR(From3132FixPt((*offsets)[2]), 0.75, 1e-4);
}

TEST(ColorUtilTest, ToColorOffsetGamutRotation) {
  HalColorTransformMatrix matrix = {
      1.0F, 0.0F, 0.0F, 0.0F,  //
      0.0F, 1.0F, 0.0F, 0.0F,  //
      0.0F, 0.0F, 1.0F, 0.0F,  //
      0.1F, 0.2F, 0.3F, 1.0F,  //
  };
  auto hal_matrix = std::make_shared<const HalColorTransformMatrix>(matrix);

  auto offsets = ColorUtil::ToColorOffset(HwcColorspace::kBt709,
                                          HwcColorspace::kBt2020, hal_matrix);
  ASSERT_NE(offsets, nullptr);

  mat3d gamut_transform(
      ColorSpaceConnector(ColorGamut::BT709(), ColorGamut::BT2020())
          .getTransform());
  double3 expected_o = gamut_transform * double3(0.1, 0.2, 0.3);

  EXPECT_NEAR(From3132FixPt((*offsets)[0]), expected_o[0], 1e-4);
  EXPECT_NEAR(From3132FixPt((*offsets)[1]), expected_o[1], 1e-4);
  EXPECT_NEAR(From3132FixPt((*offsets)[2]), expected_o[2], 1e-4);
}

TEST(ColorUtilTest, ToLinearCtmDiagonalScaling) {
  HalColorTransformMatrix night_light = {
      0.9F, 0.0F, 0.0F, 0.0F,  //
      0.0F, 0.7F, 0.0F, 0.0F,  //
      0.0F, 0.0F, 0.4F, 0.0F,  //
      0.0F, 0.0F, 0.0F, 1.0F,  //
  };

  auto linear_ctm = ColorUtil::ToLinearCtm(night_light, ColorMode::kSrgb);
  // Linearized diagonal values should differ from original (EOTF applied)
  EXPECT_NE(linear_ctm[0], night_light[0]);
  EXPECT_NE(linear_ctm[5], night_light[5]);
  EXPECT_NE(linear_ctm[10], night_light[10]);
  EXPECT_FLOAT_EQ(linear_ctm[15], 1.0F);

  // All off-diagonals should remain 0
  EXPECT_FLOAT_EQ(linear_ctm[1], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[2], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[3], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[4], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[6], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[7], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[8], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[9], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[11], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[12], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[13], 0.0F);
  EXPECT_FLOAT_EQ(linear_ctm[14], 0.0F);
}
TEST(ColorUtilTest, ToLinearCtmBypassesNonDiagonalAndPerspective) {
  // 1. Matrix with off-diagonal color cross-talk
  HalColorTransformMatrix cross_talk = {
      0.9F, 0.1F, 0.0F, 0.0F,  //
      0.1F, 0.8F, 0.0F, 0.0F,  //
      0.0F, 0.0F, 0.7F, 0.0F,  //
      0.0F, 0.0F, 0.0F, 1.0F,  //
  };
  EXPECT_EQ(ColorUtil::ToLinearCtm(cross_talk, ColorMode::kSrgb), cross_talk);

  // 2. Matrix with translation offsets (e.g. Invert Colors Y = 1 - X)
  HalColorTransformMatrix invert = {
      -1.0F, 0.0F,  0.0F,  0.0F,  //
      0.0F,  -1.0F, 0.0F,  0.0F,  //
      0.0F,  0.0F,  -1.0F, 0.0F,  //
      1.0F,  1.0F,  1.0F,  1.0F,  //
  };
  EXPECT_EQ(ColorUtil::ToLinearCtm(invert, ColorMode::kSrgb), invert);

  // 3. Matrix with non-zero perspective row entries (indices 3, 7, 11)
  HalColorTransformMatrix perspective = {
      1.0F, 0.0F, 0.0F, 0.1F,  // m[3] is non-zero
      0.0F, 1.0F, 0.0F, 0.0F,  //
      0.0F, 0.0F, 1.0F, 0.0F,  //
      0.0F, 0.0F, 0.0F, 1.0F,  //
  };
  EXPECT_EQ(ColorUtil::ToLinearCtm(perspective, ColorMode::kSrgb), perspective);

  // 4. Matrix with positive translation offsets (indices 12, 13, 14) without
  // negative values
  HalColorTransformMatrix translation = {
      1.0F, 0.0F, 0.0F, 0.0F,  //
      0.0F, 1.0F, 0.0F, 0.0F,  //
      0.0F, 0.0F, 1.0F, 0.0F,  //
      0.5F, 0.2F, 0.1F, 1.0F,  //
  };
  EXPECT_EQ(ColorUtil::ToLinearCtm(translation, ColorMode::kSrgb), translation);
}
// Tests for CreateDegammaLut
TEST(ColorUtilTest, CreateDegammaLutInvalidLutSize) {
  // Invalid size < 2
  const auto lut0 = ColorUtil::CreateDegammaLut(TransferFunction::kPq, 0, 1.0F);
  EXPECT_TRUE(lut0.empty());

  const auto lut1 = ColorUtil::CreateDegammaLut(TransferFunction::kPq, 1, 1.0F);
  EXPECT_TRUE(lut1.empty());
}

TEST(ColorUtilTest, CreateDegammaLutValid) {
  static constexpr size_t kLutSize = 256;
  const auto lut = ColorUtil::CreateDegammaLut(TransferFunction::kPq, kLutSize,
                                               1.0F);

  ASSERT_EQ(lut.size(), kLutSize);

  // Check initial and final values (linear / EOTF mapping)
  EXPECT_EQ(lut[0].red, 0U);
  EXPECT_EQ(lut[0].green, 0U);
  EXPECT_EQ(lut[0].blue, 0U);

  EXPECT_GT(lut[kLutSize - 1].red, 0U);
}

// Tests for CreateGammaLut
TEST(ColorUtilTest, CreateGammaLutInvalidLutSize) {
  const auto lut = ColorUtil::CreateGammaLut(TransferFunction::kHlg, 1, 1.0F,
                                             1.0F);
  EXPECT_TRUE(lut.empty());
}

TEST(ColorUtilTest, CreateGammaLutValid) {
  static constexpr size_t kLutSize = 512;
  const auto lut = ColorUtil::CreateGammaLut(TransferFunction::kHlg, kLutSize,
                                             1.0F, 1.0F);

  ASSERT_EQ(lut.size(), kLutSize);

  EXPECT_EQ(lut[0].red, 0U);
  EXPECT_EQ(lut[0].green, 0U);
  EXPECT_EQ(lut[0].blue, 0U);

  EXPECT_GT(lut[kLutSize - 1].red, 0U);
}

TEST(ColorUtilTest, CreateGammaLutHdrClampsToMinFloorAtZeroDisplayBrightness) {
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.01");

  static constexpr size_t kLutSize = 512;
  const auto lut = ColorUtil::CreateGammaLut(TransferFunction::kHlg, kLutSize,
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

TEST(ColorUtilTest, CreateGammaLutHdrScaleRangeNoopWhenMinZero) {
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  static constexpr size_t kLutSize = 512;
  const auto lut = ColorUtil::CreateGammaLut(TransferFunction::kHlg, kLutSize,
                                             ColorUtil::ScaleBrightnessIfNeeded(
                                                 0.0F),
                                             1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_EQ(lut[kLutSize - 1].red, 0U);
  EXPECT_EQ(lut[kLutSize - 1].green, 0U);
  EXPECT_EQ(lut[kLutSize - 1].blue, 0U);
}

TEST(ColorUtilTest, CreateGammaLutHdrScaleRangeNonZeroAtZero) {
  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  static constexpr size_t kLutSize = 512;
  const auto lut = ColorUtil::CreateGammaLut(TransferFunction::kHlg, kLutSize,
                                             ColorUtil::ScaleBrightnessIfNeeded(
                                                 0.0F),
                                             1.0F);

  ASSERT_EQ(lut.size(), kLutSize);
  EXPECT_GT(lut[kLutSize - 1].red, 0U);
  EXPECT_GT(lut[kLutSize - 1].green, 0U);
  EXPECT_GT(lut[kLutSize - 1].blue, 0U);
}

TEST(ColorUtilTest, CreateGammaLutHdrScaleRangeCalculatesExpectedScale) {
  static constexpr size_t kLutSize = 512;

  const auto lut_scaled = [&]() {
    ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
    ScopedTestProperty
        scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                   "true");
    return ColorUtil::CreateGammaLut(TransferFunction::kHlg, kLutSize,
                                     ColorUtil::ScaleBrightnessIfNeeded(0.5F),
                                     1.0F);
  }();

  const auto lut_expected = ColorUtil::CreateGammaLut(TransferFunction::kHlg,
                                                      kLutSize, 0.55F, 1.0F);

  EXPECT_EQ(lut_scaled.size(), lut_expected.size());
  for (size_t i = 0; i < kLutSize; ++i) {
    EXPECT_EQ(lut_scaled[i].red, lut_expected[i].red);
    EXPECT_EQ(lut_scaled[i].green, lut_expected[i].green);
    EXPECT_EQ(lut_scaled[i].blue, lut_expected[i].blue);
  }
}

TEST(ColorUtilTest, CalculateDegammaScale) {
  EXPECT_FLOAT_EQ(ColorUtil::CalculateDegammaScale(std::nullopt), 1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateDegammaScale(0.0F), 1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateDegammaScale(1.0F), 1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateDegammaScale(-0.5F), 1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateDegammaScale(1.5F), 1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateDegammaScale(0.42F), 0.42F);
}

TEST(ColorUtilTest, CalculateGammaScale) {
  EXPECT_FLOAT_EQ(ColorUtil::CalculateGammaScale(std::nullopt, std::nullopt),
                  1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateGammaScale(0.0F, 1.0F), 0.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateGammaScale(0.5F, 1.0F), 0.5F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateGammaScale(0.5F, 0.8F), 0.4F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateGammaScale(1.0F, 1.0F), 1.0F);
  EXPECT_FLOAT_EQ(ColorUtil::CalculateGammaScale(0.5F, 1.5F), 0.5F);
}

TEST(ColorUtilTest, ToColorGamutMappings) {
  EXPECT_EQ(ColorUtil::ToColorGamut(HwcColorspace::kDefault).getName(),
            android::ColorSpace::BT709().getName());
  EXPECT_EQ(ColorUtil::ToColorGamut(HwcColorspace::kBt709).getName(),
            android::ColorSpace::BT709().getName());
  EXPECT_EQ(ColorUtil::ToColorGamut(HwcColorspace::kBt2020).getName(),
            android::ColorSpace::BT2020().getName());
  EXPECT_EQ(ColorUtil::ToColorGamut(HwcColorspace::kDciP3).getName(),
            android::ColorSpace::DCIP3().getName());
  EXPECT_EQ(ColorUtil::ToColorGamut(HwcColorspace::kBt601).getName(),
            android::ColorSpace::sRGB().getName());
}

TEST(ColorUtilTest, ToHwcColorspaceManyToOneMappings) {
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kNative),
            HwcColorspace::kDefault);

  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt601_625),
            HwcColorspace::kBt601);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt601_625Unadjusted),
            HwcColorspace::kBt601);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt601_525),
            HwcColorspace::kBt601);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt601_525Unadjusted),
            HwcColorspace::kBt601);

  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kSrgb),
            HwcColorspace::kBt709);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt709),
            HwcColorspace::kBt709);

  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kDciP3),
            HwcColorspace::kDciP3);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kDisplayP3),
            HwcColorspace::kDciP3);

  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt2020),
            HwcColorspace::kBt2020);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kDisplayBt2020),
            HwcColorspace::kBt2020);

  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kAdobeRgb),
            HwcColorspace::kDefault);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt2100Pq),
            HwcColorspace::kDefault);
  EXPECT_EQ(ColorUtil::ToHwcColorspace(ColorMode::kBt2100Hlg),
            HwcColorspace::kDefault);
}

TEST(ColorUtilTest, GetEotfManyToOneMappings) {
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kSrgb)(0.5F),
                  android::ColorSpace::sRGB().getEOTF()(0.5F));
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kBt601_625)(0.5F),
                  android::ColorSpace::sRGB().getEOTF()(0.5F));
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kBt601_625Unadjusted)(0.5F),
                  android::ColorSpace::sRGB().getEOTF()(0.5F));
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kBt601_525)(0.5F),
                  android::ColorSpace::sRGB().getEOTF()(0.5F));
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kBt601_525Unadjusted)(0.5F),
                  android::ColorSpace::sRGB().getEOTF()(0.5F));

  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kBt709)(0.5F),
                  android::ColorSpace::BT709().getEOTF()(0.5F));

  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kDciP3)(0.5F),
                  android::ColorSpace::DCIP3().getEOTF()(0.5F));
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kDisplayP3)(0.5F),
                  android::ColorSpace::DCIP3().getEOTF()(0.5F));

  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kBt2020)(0.5F),
                  android::ColorSpace::BT2020().getEOTF()(0.5F));
  EXPECT_FLOAT_EQ(ColorUtil::GetEotf(ColorMode::kDisplayBt2020)(0.5F),
                  android::ColorSpace::BT2020().getEOTF()(0.5F));
}

TEST(ColorUtilTest, ToLinearCtmManyToOneMappings) {
  constexpr HalColorTransformMatrix kCtm = {
      0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F,
      0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
  };

  EXPECT_FLOAT_EQ(ColorUtil::ToLinearCtm(kCtm, ColorMode::kBt601_625)[0],
                  ColorUtil::ToLinearCtm(kCtm, ColorMode::kSrgb)[0]);
  EXPECT_FLOAT_EQ(ColorUtil::ToLinearCtm(kCtm,
                                         ColorMode::kBt601_625Unadjusted)[0],
                  ColorUtil::ToLinearCtm(kCtm, ColorMode::kSrgb)[0]);
  EXPECT_FLOAT_EQ(ColorUtil::ToLinearCtm(kCtm, ColorMode::kBt601_525)[0],
                  ColorUtil::ToLinearCtm(kCtm, ColorMode::kSrgb)[0]);
  EXPECT_FLOAT_EQ(ColorUtil::ToLinearCtm(kCtm,
                                         ColorMode::kBt601_525Unadjusted)[0],
                  ColorUtil::ToLinearCtm(kCtm, ColorMode::kSrgb)[0]);

  EXPECT_FLOAT_EQ(ColorUtil::ToLinearCtm(kCtm, ColorMode::kDciP3)[0],
                  ColorUtil::ToLinearCtm(kCtm, ColorMode::kDisplayP3)[0]);

  EXPECT_FLOAT_EQ(ColorUtil::ToLinearCtm(kCtm, ColorMode::kBt2020)[0],
                  ColorUtil::ToLinearCtm(kCtm, ColorMode::kDisplayBt2020)[0]);
}

TEST(ColorUtilTest, To3132FixPtBasics) {
  EXPECT_EQ(ColorUtil::To3132FixPt(0.0), 0ULL);
  EXPECT_EQ(ColorUtil::To3132FixPt(1.0), 1ULL << 32);
  EXPECT_EQ(ColorUtil::To3132FixPt(2.0), 2ULL << 32);
  EXPECT_EQ(ColorUtil::To3132FixPt(0.5), 1ULL << 31);
  EXPECT_EQ(ColorUtil::To3132FixPt(0.25), 1ULL << 30);
  EXPECT_EQ(ColorUtil::To3132FixPt(0.125), 1ULL << 29);

  constexpr uint64_t kSignBit = 1ULL << 63;
  EXPECT_EQ(ColorUtil::To3132FixPt(-1.0), kSignBit | (1ULL << 32));
  EXPECT_EQ(ColorUtil::To3132FixPt(-2.0), kSignBit | (2ULL << 32));
  EXPECT_EQ(ColorUtil::To3132FixPt(-0.5), kSignBit | (1ULL << 31));

  // Smallest representable non-zero step: 2^-32
  constexpr double kQuantum = 1.0 / 4294967296.0;
  EXPECT_EQ(ColorUtil::To3132FixPt(kQuantum), 1ULL);
  EXPECT_EQ(ColorUtil::To3132FixPt(-kQuantum), kSignBit | 1ULL);
}

TEST(ColorUtilTest, To3132FixPtRounding) {
  constexpr double kQuantum = 1.0 / 4294967296.0;

  // 0.4 * quantum should round down to 0
  EXPECT_EQ(ColorUtil::To3132FixPt(0.4 * kQuantum), 0ULL);

  // 0.6 * quantum should round up to 1
  EXPECT_EQ(ColorUtil::To3132FixPt(0.6 * kQuantum), 1ULL);

  // 1.4 * quantum should round to 1
  EXPECT_EQ(ColorUtil::To3132FixPt(1.4 * kQuantum), 1ULL);

  // 1.6 * quantum should round to 2
  EXPECT_EQ(ColorUtil::To3132FixPt(1.6 * kQuantum), 2ULL);
}

TEST(ColorUtilTest, To3132FixPtSaturationAndSpecialValues) {
  constexpr uint64_t kSignBit = 1ULL << 63;
  constexpr uint64_t kValueMask = (1ULL << 63) - 1;

  // Values exceeding (2^31 - 1) saturate at maximum magnitude
  EXPECT_EQ(ColorUtil::To3132FixPt(3e9), kValueMask);
  EXPECT_EQ(ColorUtil::To3132FixPt(-3e9), kSignBit | kValueMask);

  // Large infinity saturates
  EXPECT_EQ(ColorUtil::To3132FixPt(std::numeric_limits<double>::infinity()),
            kValueMask);
  EXPECT_EQ(ColorUtil::To3132FixPt(-std::numeric_limits<double>::infinity()),
            kSignBit | kValueMask);

  // NaN returns 0
  EXPECT_EQ(ColorUtil::To3132FixPt(std::numeric_limits<double>::quiet_NaN()),
            0ULL);

  // Negative zero
  EXPECT_EQ(ColorUtil::To3132FixPt(-0.0), kSignBit);
}

TEST(ColorUtilTest, ToDrmColorspaceMappings) {
  EXPECT_EQ(ColorUtil::ToDrmColorspace(HwcColorspace::kDefault),
            DrmColorspace::kDefault);
  EXPECT_EQ(ColorUtil::ToDrmColorspace(HwcColorspace::kBt601),
            DrmColorspace::kDefault);
  EXPECT_EQ(ColorUtil::ToDrmColorspace(HwcColorspace::kBt709),
            DrmColorspace::kDefault);
  EXPECT_EQ(ColorUtil::ToDrmColorspace(HwcColorspace::kDciP3),
            DrmColorspace::kDciP3RgbD65);
  EXPECT_EQ(ColorUtil::ToDrmColorspace(HwcColorspace::kBt2020),
            DrmColorspace::kBt2020Rgb);
}

}  // namespace android::drm_hwcomposer

// NOLINTEND(readability-magic-numbers)