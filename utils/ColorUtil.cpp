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

#include "ColorUtil.h"

#include <ui/ColorSpace.h>

#include "compositor/DisplayInfo.h"
#include "math/TMatHelpers.h"

using ColorGamut = android::ColorSpace;

namespace android::drm_hwcomposer {

namespace {
uint64_t To3132FixPt(float in) {
  constexpr uint64_t kSignMask = (1ULL << 63);
  constexpr uint64_t kValueMask = ~(1ULL << 63);
  constexpr auto kValueScale = static_cast<float>(1ULL << 32);
  if (in < 0)
    return (static_cast<uint64_t>(-in * kValueScale) & kValueMask) | kSignMask;
  return static_cast<uint64_t>(in * kValueScale) & kValueMask;
}

template <typename T>
std::shared_ptr<T> ToColorTransform(
    const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix,
    const bool output_is_3x4_matrix) {
  const HalColorTransforMatrix &hal_ctm = color_transform_matrix
                                              ? *color_transform_matrix
                                              : kIdentityMatrix;
  std::shared_ptr<T> color_matrix = std::make_shared<T>();
  const int rows = output_is_3x4_matrix ? 4 : 3;
  constexpr int kCols = 3;
  constexpr int kHalRows = 4;
  for (int i = 0; i < kCols; i++) {
    for (int j = 0; j < rows; j++) {
      color_matrix->matrix[(i * rows) + j] = To3132FixPt(
          hal_ctm[(j * kHalRows) + i]);
    }
  }
  return color_matrix;
}

ColorGamut ToColorGamut(Colorspace colorspace) {
  switch (colorspace) {
    case Colorspace::kBt709Ycc:
    case Colorspace::kXvycc709:
      return ColorGamut::BT709();
    case Colorspace::kBt2020Cycc:
    case Colorspace::kBt2020Rgb:
    case Colorspace::kBt2020Ycc:
      return ColorGamut::BT2020();
    case Colorspace::kDciP3RgbD65:
    case Colorspace::kDciP3RgbTheater:
      return ColorGamut::DCIP3();
    case Colorspace::kDefault:
    case Colorspace::kSycc601:
    case Colorspace::kOpycc601:
    case Colorspace::kBt601Ycc:
    case Colorspace::kOprgb:
    case Colorspace::kRgbWideFixed:
    case Colorspace::kSmpte170MYcc:
    case Colorspace::kRgbWideFloat:
    default:
      return ColorGamut::sRGB();
  }
}

}  // namespace

std::shared_ptr<drm_color_ctm> ColorUtil::ToColorTransform3x3(
    const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix) {
  return ToColorTransform<drm_color_ctm>(color_transform_matrix,
                                         /*output_is_3x4_matrix=*/false);
}

std::shared_ptr<drm_color_ctm_3x4> ColorUtil::ToColorTransform3x4(
    const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix) {
  return ToColorTransform<drm_color_ctm_3x4>(color_transform_matrix,
                                             /*output_is_3x4_matrix=*/true);
}

std::shared_ptr<drm_color_ctm_3x4> ColorUtil::ToColorTransform3x4(
    const android::mat4 &color_transform_matrix) {
  auto color_matrix = std::make_shared<drm_color_ctm_3x4>();
  constexpr int kRows = 4;
  constexpr int kCols = 3;
  for (int i = 0; i < kCols; i++) {
    for (int j = 0; j < kRows; j++) {
      color_matrix->matrix[(i * kRows) + j] = To3132FixPt(
          color_transform_matrix[j][i]);
    }
  }
  return color_matrix;
}

std::shared_ptr<drm_color_ctm_3x4> ColorUtil::GamutAdjustIfNeeded(
    Colorspace src_colorspace, Colorspace dest_colorspace,
    const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix,
    std::map<std::tuple<Colorspace, Colorspace>, const mat3>
        &color_transform_cache) {
  if (src_colorspace == dest_colorspace) {
    return ColorUtil::ToColorTransform3x4(color_transform_matrix);
  }

  const HalColorTransforMatrix &ctm_in = color_transform_matrix
                                             ? *color_transform_matrix
                                             : kIdentityMatrix;
  // Extract the inner 3x3 matrix from the 4x4 CTM
  // NOLINTBEGIN(readability-magic-numbers)
  // clang-format off
  mat3 ctm3(
    ctm_in[0], ctm_in[1], ctm_in[2],
    ctm_in[4], ctm_in[5], ctm_in[6],
    ctm_in[8], ctm_in[9], ctm_in[10]
  );
  // clang-format on
  // NOLINTEND(readability-magic-numbers)

  // Combine the gamut mapping with 3x3 CTM
  auto cache_key = std::tie(src_colorspace, dest_colorspace);
  if (color_transform_cache.count(cache_key) == 0) {
    color_transform_cache
        .emplace(cache_key, ColorSpaceConnector(ToColorGamut(src_colorspace),
                                                ToColorGamut(dest_colorspace))
                                .getTransform());
  }
  ctm3 = color_transform_cache.at(cache_key) * ctm3;

  // Insert the new 3x3 matrix back into the 4x4 CTM
  // NOLINTBEGIN(readability-magic-numbers)
  // clang-format off
  mat4 ctm4 = mat4(
    ctm3[0][0], ctm3[0][1], ctm3[0][2], ctm_in[3],
    ctm3[1][0], ctm3[1][1], ctm3[1][2], ctm_in[7],
    ctm3[2][0], ctm3[2][1], ctm3[2][2], ctm_in[11],
    ctm_in[12], ctm_in[13], ctm_in[14], ctm_in[15]
  );
  // clang-format on
  // NOLINTEND(readability-magic-numbers)

  return ToColorTransform3x4(ctm4);
}

}  // namespace android::drm_hwcomposer
