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
#include <math/mat4.h>
#include <ui/ColorSpace.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/DrmColorspace.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

using ColorGamut = ::android::ColorSpace;

template <typename T>
using Lut1D = std::vector<T>;

// Static ColorGamut instances avoid recomputing color space matrix inverses
// and std::function binders on every invocation, and ensure transfer function
// references never dangle.
inline const ColorGamut kSrgbGamut = ColorGamut::sRGB();
inline const ColorGamut kBt709Gamut = ColorGamut::BT709();
inline const ColorGamut kDciP3Gamut = ColorGamut::DCIP3();
inline const ColorGamut kBt2020Gamut = ColorGamut::BT2020();

inline constexpr float kSdrReferenceWhiteLevel = 203.F;
inline constexpr float kHdrReferenceLuminance = 10000.F;
inline constexpr float kDefaultMaxLuminance = 500.F;

inline constexpr double kSignalMin = 0.0;
inline constexpr double kSignalMax = 1.0;

inline constexpr float kMinBrightness = 0.0F;
inline constexpr float kMaxBrightness = 1.0F;

static constexpr size_t kHdrBoostLutSize = 2048;

class ColorUtil {
 public:
  /* Evaluates the tone-mapping multiplier for a linear signal level. */
  // NOLINTBEGIN(readability-magic-numbers)
  static constexpr double BoostHdrMultiplier(double signal) {
    constexpr double kShadowBoost = 3.0;
    constexpr double kShadowLimit = kSdrReferenceWhiteLevel /
                                    kHdrReferenceLuminance;
    constexpr double kHighlightBaseline = 0.32 / kShadowLimit;
    double shadow_fraction = std::clamp(1.0 - (signal / kShadowLimit), 0.0,
                                        1.0);
    double multiplier = 1.0 +
                        (kShadowBoost * shadow_fraction * shadow_fraction *
                         shadow_fraction * shadow_fraction);
    return multiplier * kHighlightBaseline;
  }
  // NOLINTEND(readability-magic-numbers)

  /* Generator for the 1D HDR Highlight & Shadow Tone-Mapping Boost LUT. */
  static constexpr std::array<float, kHdrBoostLutSize> GenerateHdrBoostLut() {
    std::array<float, kHdrBoostLutSize> lut{};
    for (size_t i = 0; i < kHdrBoostLutSize; ++i) {
      auto signal = static_cast<double>(i) /
                    static_cast<double>(kHdrBoostLutSize - 1);
      lut[i] = static_cast<float>(BoostHdrMultiplier(signal));
    }
    return lut;
  }

  /* Precomputed 2048-entry table stored in read-only memory */
  static inline const std::array<float, kHdrBoostLutSize>
      kHdrBoostLut = ColorUtil::GenerateHdrBoostLut();

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
      const std::shared_ptr<const HalColorTransformMatrix>
          &color_transform_matrix);

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
      const std::shared_ptr<const HalColorTransformMatrix>
          &color_transform_matrix);

  static bool HasOffset(const HalColorTransformMatrix &matrix);

  static std::shared_ptr<std::array<uint64_t, 3>> ToColorOffset(
      const std::shared_ptr<const HalColorTransformMatrix>
          &color_transform_matrix);

  static std::shared_ptr<std::array<uint64_t, 3>> ToColorOffset(
      HwcColorspace src_colorspace, HwcColorspace dest_colorspace,
      const std::shared_ptr<const HalColorTransformMatrix>
          &color_transform_matrix);

  static std::shared_ptr<const HalColorTransformMatrix> Multiply(
      const std::shared_ptr<const HalColorTransformMatrix> &a,
      const std::shared_ptr<const HalColorTransformMatrix> &b);

  static HwcColorspace ToHwcColorspace(ColorMode mode) {
    switch (mode) {
      case ColorMode::kNative:
        return HwcColorspace::kDefault;
      case ColorMode::kBt601_625:
      case ColorMode::kBt601_625Unadjusted:
      case ColorMode::kBt601_525:
      case ColorMode::kBt601_525Unadjusted:
        return HwcColorspace::kBt601;
      case ColorMode::kSrgb:
      case ColorMode::kBt709:
        return HwcColorspace::kBt709;
      case ColorMode::kDciP3:
      case ColorMode::kDisplayP3:
        return HwcColorspace::kDciP3;
      case ColorMode::kBt2020:
      case ColorMode::kDisplayBt2020:
        return HwcColorspace::kBt2020;
      case ColorMode::kAdobeRgb:
      case ColorMode::kBt2100Pq:
      case ColorMode::kBt2100Hlg:
        ALOGW("Unsupported color mode: %d", static_cast<int32_t>(mode));
        return HwcColorspace::kDefault;
    }
  }

  static DrmColorspace ToDrmColorspace(HwcColorspace colorspace) {
    switch (colorspace) {
      case HwcColorspace::kDefault:
      case HwcColorspace::kBt601:
      case HwcColorspace::kBt709:
        return DrmColorspace::kDefault;
      case HwcColorspace::kDciP3:
        return DrmColorspace::kDciP3RgbD65;
      case HwcColorspace::kBt2020:
        return DrmColorspace::kBt2020Rgb;
      default:
        ALOGW("Unknown HwcColorspace %d, falling back to kDefault",
              static_cast<int>(colorspace));
        return DrmColorspace::kDefault;
    }
  }

  static const ColorGamut &ToColorGamut(HwcColorspace colorspace);

  static const ColorGamut::transfer_function &GetEotf(ColorMode mode);

  static uint64_t To3132FixPt(double in);

  static const char *ToString(TransferFunction tf) {
    switch (tf) {
      case TransferFunction::kSmpte170M:
        return "SMPTE 170M";
      case TransferFunction::kSrgb:
        return "sRGB";
      case TransferFunction::kPq:
        return "PQ";
      case TransferFunction::kHlg:
        return "HLG";
      case TransferFunction::kUnknown:
        [[fallthrough]];
      default:
        return "Unknown";
    }
  }

  static const char *ToString(HwcColorspace cs) {
    switch (cs) {
      case HwcColorspace::kBt601:
        return "BT.601";
      case HwcColorspace::kBt709:
        return "BT.709";
      case HwcColorspace::kDciP3:
        return "DCI-P3";
      case HwcColorspace::kBt2020:
        return "BT.2020";
      case HwcColorspace::kDefault:
        [[fallthrough]];
      default:
        return "Default";
    }
  }

  /* Framework sends CTM assuming non-linear input. Transform must be converted
   * to a linear matrix to be applied correctly in the color pipeline.
   */
  static HalColorTransformMatrix ToLinearCtm(HalColorTransformMatrix ctm,
                                             ColorMode mode);

  // If required, adjust color transform matrix to handle gamut mapping
  template <typename T>
  static std::shared_ptr<T> GamutAdjustIfNeeded(
      HwcColorspace src_colorspace, HwcColorspace dest_colorspace,
      const std::shared_ptr<const HalColorTransformMatrix>
          &color_transform_matrix);

  static float CalculateDegammaScale(
      std::optional<float> layer_brightness = std::nullopt) {
    constexpr float kDefaultSignal = 1.F;
    float brightness = layer_brightness.value_or(kDefaultSignal);
    if (brightness > kSignalMin && brightness < kSignalMax) {
      return brightness;
    }
    return static_cast<float>(kSignalMax);
  }

  static float CalculateGammaScale(
      std::optional<float> display_brightness = std::nullopt,
      std::optional<float> hdr_headroom = std::nullopt) {
    auto lut_scale = static_cast<float>(kSignalMax);
    if (display_brightness.has_value() && *display_brightness >= kSignalMin &&
        *display_brightness < kSignalMax) {
      lut_scale = *display_brightness;
    }
    if (hdr_headroom.has_value() && *hdr_headroom > kSignalMin &&
        *hdr_headroom < kSignalMax) {
      lut_scale *= *hdr_headroom;
    }
    return lut_scale;
  }

  /* Creates 1D Gamma/Degamma LUTs using the appropriate OETF/EOTF for the given
   * transfer function. If an invalid LUT size (< 2) is requested, returns an
   * empty array.
   */
  static Lut1D<drm_color_lut32> CreateDegammaLut(
      TransferFunction tf, size_t lut_size,
      std::optional<float> layer_brightness = std::nullopt);
  static Lut1D<drm_color_lut> CreateGammaLut(
      TransferFunction tf, size_t lut_size,
      std::optional<float> display_brightness = std::nullopt,
      std::optional<float> hdr_headroom = std::nullopt);
  /**
   * Applies the minimum display brightness floor.
   */
  static auto ScaleBrightnessIfNeeded(float display_brightness) -> float;

  static bool NeedsTonemapping(TransferFunction tf);

  static std::vector<drm_color_lut32> GetLogGammaLut(
      TransferFunction tf,
      std::optional<float> display_brightness = std::nullopt,
      std::optional<float> hdr_headroom = std::nullopt);

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
