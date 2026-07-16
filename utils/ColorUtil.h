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

#pragma once

#include <drm/drm_mode.h>
#include <math/mat3.h>
#include <math/mat4.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

template <typename T>
using Lut1D = std::vector<T>;
template <typename T>
using Lut1DCache = std::map<std::tuple<TransferFunction, size_t, float>,
                            Lut1D<T>>;
using CscCache = std::map<std::tuple<Colorspace, Colorspace>, const mat3d>;

template <typename T>
inline const Lut1D<T> kEmptyLut = {};

inline constexpr float kHdrReferenceLuminance = 10000.F;
inline constexpr float kDefaultMaxLuminance = 500.F;

inline constexpr double kSignalMin = 0.0;
inline constexpr double kSignalMax = 1.0;

class ColorUtil {
 public:
  /**
   * Keep in sync with the Dataspace.aidl ARIB STD-B67 Hybrid Log Gamma (HLG)
   * definition
   * https://cs.android.com/android/platform/superproject/main/+/main:hardware/interfaces/graphics/common/aidl/android/hardware/graphics/common/Dataspace.aidl;l=348;drc=dbf753b896a75f3e712bc362a01763d731e49f57
   */
  static double EvaluateHlgOetf(double l);

  /* HAL provides a transposed 4x4 float type matrix:
   * | 0  1  2  3|
   * | 4  5  6  7|
   * | 8  9 10 11|
   * |12 13 14 15|
   *
   * R_out = R*0 + G*4 + B*8 + 12
   * G_out = R*1 + G*5 + B*9 + 13
   * B_out = R*2 + G*6 + B*10 + 14
   *
   * drm_color_ctm expects a 3x3 s31.32 fixed point matrix:
   * out   matrix    in
   * |R|   |0 1 2|   |R|
   * |G| = |3 4 5| x |G|
   * |B|   |6 7 8|   |B|
   *
   * R_out = R*0 + G*1 + B*2
   * G_out = R*3 + G*4 + B*5
   * B_out = R*6 + G*7 + B*8
   */
  static std::shared_ptr<drm_color_ctm> ToColorTransform3x3(
      const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix);

  /* HAL provides a transposed 4x4 float type matrix:
   * | 0  1  2  3|
   * | 4  5  6  7|
   * | 8  9 10 11|
   * |12 13 14 15|
   *
   * R_out = R*0 + G*4 + B*8 + 12
   * G_out = R*1 + G*5 + B*9 + 13
   * B_out = R*2 + G*6 + B*10 + 14
   *
   * drm_color_ctm_3x4 expects a 3x4 s31.32 fixed point matrix:
   * out   matrix          in
   * |R|   |0  1  2  3 |   | R |
   * |G| = |4  5  6  7 | x | G |
   * |B|   |8  9  10 11|   | B |
   *                       |1.0|
   *
   * R_out = R*0 + G*1 + B*2 + 3
   * G_out = R*4 + G*5 + B*6 + 7
   * B_out = R*8 + G*9 + B*10 + 11
   */
  static std::shared_ptr<drm_color_ctm_3x4> ToColorTransform3x4(
      const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix);

  /* Maps to the Colorspace DRM connector property:
   * https://elixir.bootlin.com/linux/v6.11/source/include/drm/drm_connector.h#L538
   */
  static Colorspace ToColorspace(ColorMode mode) {
    switch (mode) {
      case ColorMode::kNative:
        return Colorspace::kDefault;
      case ColorMode::kBt601_625:
      case ColorMode::kBt601_625Unadjusted:
      case ColorMode::kBt601_525:
      case ColorMode::kBt601_525Unadjusted:
        // The DP spec does not say whether this is the 525 or the 625 line
        // version.
        return Colorspace::kBt601Ycc;
      case ColorMode::kBt709:
      case ColorMode::kSrgb:
        return Colorspace::kBt709Ycc;
      case ColorMode::kDciP3:
      case ColorMode::kDisplayP3:
        return Colorspace::kDciP3RgbD65;
      case ColorMode::kBt2020:
      case ColorMode::kDisplayBt2020:
        return Colorspace::kBt2020Rgb;
      case ColorMode::kAdobeRgb:
      case ColorMode::kBt2100Pq:
      case ColorMode::kBt2100Hlg:
        ALOGW("Unsupported color mode: %d", static_cast<int32_t>(mode));
        return Colorspace::kDefault;
    }
  }

  // If required, adjust color transform matrix to handle gamut mapping
  template <typename T>
  static std::shared_ptr<T> GamutAdjustIfNeeded(
      Colorspace src_colorspace, Colorspace dest_colorspace,
      const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix,
      CscCache &color_transform_cache);

  /* Creates 1D Gamma/Degamma LUTs using the appropriate OETF/EOTF for the given
   * transfer function, adds it to the lut_1d_map and returns the map element
   * reference. If a LUT has already been generated for this set of inputs, it
   * returns that LUT from the mapping. If no LUT is generated, returns an empty
   * array reference.
   */
  static const Lut1D<drm_color_lut32> &GetDegammaLut(
      TransferFunction dest_tf, TransferFunction src_tf, size_t lut_size,
      Lut1DCache<drm_color_lut32> &lut_1d_map, float layer_brightness);
  static const Lut1D<drm_color_lut> &GetGammaLut(
      TransferFunction tf, size_t lut_size,
      Lut1DCache<drm_color_lut> &lut_1d_map, float display_brightness,
      float hdr_headroom);

 private:
  /* Converts a column-major 4x4 float type flat array matrix into
   * row-major a 3x4 s31.32 fixed point flat array matrix.
   * in:           -> out:
   * | 0  1  2  3|    |0  4  8  12|
   * | 4  5  6  7|    |1  5  9  13|
   * | 8  9 10 11| -> |2  6  10 14|
   * |12 13 14 15|
   */
  static std::shared_ptr<drm_color_ctm_3x4> ToColorTransform3x4(
      const android::mat4d &color_transform_matrix);
};

}  // namespace android::drm_hwcomposer
