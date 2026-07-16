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

#include <drm/drm_mode.h>
#include <math/TMatHelpers.h>
#include <math/mat3.h>
#include <math/mat4.h>
#include <ui/ColorSpace.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "utils/log.h"

using ColorGamut = android::ColorSpace;

namespace android::drm_hwcomposer {

namespace {

// Normalize to the range [0, 12] rather than [0, 1]
const double kHlgScale = 12.0;

uint64_t To3132FixPt(double in) {
  constexpr uint64_t kSignMask = (1ULL << 63);
  constexpr uint64_t kValueMask = ~(1ULL << 63);
  constexpr auto kValueScale = static_cast<double>(1ULL << 32);
  const double in_scaled = in * kValueScale;
  if (in < 0)
    return (static_cast<uint64_t>(-in_scaled) & kValueMask) | kSignMask;
  return static_cast<uint64_t>(in_scaled) & kValueMask;
}

template <typename T>
std::shared_ptr<T> ToColorTransform(
    const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix,
    const bool output_is_3x4_matrix) {
  if (!color_transform_matrix)
    return nullptr;

  std::shared_ptr<T> color_matrix = std::make_shared<T>();
  const int rows = output_is_3x4_matrix ? 4 : 3;
  constexpr int kCols = 3;
  constexpr int kHalRows = 4;
  for (int i = 0; i < kCols; i++) {
    for (int j = 0; j < rows; j++) {
      color_matrix->matrix[(i * rows) + j] = To3132FixPt(
          (*color_transform_matrix)[(j * kHalRows) + i]);
    }
  }
  return color_matrix;
}

std::shared_ptr<drm_color_ctm> ToColorTransform3x3(
    const android::mat3d &color_transform_matrix) {
  auto color_matrix = std::make_shared<drm_color_ctm>();
  constexpr int kDim = 3;
  for (int i = 0; i < kDim; i++) {
    for (int j = 0; j < kDim; j++) {
      color_matrix->matrix[(i * kDim) + j] = To3132FixPt(
          color_transform_matrix[j][i]);
    }
  }
  return color_matrix;
}

ColorGamut ToColorGamut(Colorspace colorspace) {
  switch (colorspace) {
    case Colorspace::kBt709Ycc:
    case Colorspace::kXvycc709:
    case Colorspace::kDefault:
      return ColorGamut::BT709();
    case Colorspace::kBt2020Cycc:
    case Colorspace::kBt2020Rgb:
    case Colorspace::kBt2020Ycc:
      return ColorGamut::BT2020();
    case Colorspace::kDciP3RgbD65:
    case Colorspace::kDciP3RgbTheater:
      return ColorGamut::DCIP3();
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

bool NeedsTonemapping(TransferFunction tf) {
  switch (tf) {
    case TransferFunction::kPq:
      [[fallthrough]];
    case TransferFunction::kHlg:
      return true;
    case TransferFunction::kSmpte170M:
      [[fallthrough]];
    case TransferFunction::kSrgb:
      [[fallthrough]];
    case TransferFunction::kUnknown:
      [[fallthrough]];
    default:
      return false;
  }
}

/**
 * Keep in sync with the Dataspace.aidl SMPTE ST 2084 (Dolby Perceptual
 * Quantizer) definition
 *
 * Transfer characteristic curve:
 *  E = ((c1 + c2 * L^n) / (1 + c3 * L^n)) ^ m
 *  c1 = c3 - c2 + 1 = 3424 / 4096 = 0.8359375
 *  c2 = 32 * 2413 / 4096 = 18.8515625
 *  c3 = 32 * 2392 / 4096 = 18.6875
 *  m = 128 * 2523 / 4096 = 78.84375
 *  n = 0.25 * 2610 / 4096 = 0.1593017578125
 *      L - luminance of image 0 <= L <= 1 for HDR colorimetry.
 *          L = 1 corresponds to 10000 cd/m2
 *      E - corresponding electrical signal
 * https://cs.android.com/android/platform/superproject/main/+/main:hardware/interfaces/graphics/common/aidl/android/hardware/graphics/common/Dataspace.aidl;l=332;drc=dbf753b896a75f3e712bc362a01763d731e49f57
 */
struct PqConstants {
  double n, m, c1, c2, c3;
};
const PqConstants kPq = {.n = 0.1593017578125,
                         .m = 78.84375,
                         .c1 = 0.8359375,
                         .c2 = 18.8515625,
                         .c3 = 18.6875};
// NOLINTBEGIN(readability-magic-numbers)
double EvaluatePqOetf(double l) {
  if (l <= kSignalMin)
    return kSignalMin;
  double l1 = pow(l, kPq.n);
  double base = (kPq.c1 + (kPq.c2 * l1)) / (1.0 + (kPq.c3 * l1));
  return pow(base, kPq.m);
}
double EvaluatePqEotf(double e) {
  if (e <= kSignalMin)
    return kSignalMin;
  double em = pow(e, 1.0 / kPq.m);
  double base = (std::max(em - kPq.c1, kSignalMin)) / (kPq.c2 - (kPq.c3 * em));
  return pow(base, 1.0 / kPq.n);
}

/**
 * Keep in sync with the Dataspace.aidl ARIB STD-B67 Hybrid Log Gamma (HLG)
 * definition
 *
 * Transfer characteristic curve:
 *  E = r * L^0.5                 for 0 <= L <= 1
 *    = a * ln(L - b) + c         for 1 < L
 *  a = 0.17883277
 *  b = 0.28466892
 *  c = 0.55991073
 *  r = 0.5
 *      L - luminance of image 0 <= L for HDR colorimetry. L = 1 corresponds
 *          to reference white level of 100 cd/m2
 *      E - corresponding electrical signal
 * https://cs.android.com/android/platform/superproject/main/+/main:hardware/interfaces/graphics/common/aidl/android/hardware/graphics/common/Dataspace.aidl;l=348;drc=dbf753b896a75f3e712bc362a01763d731e49f57
 */
struct HlgConstants {
  double a, b, c, r;
};
const HlgConstants kHlg = {.a = 0.17883277,
                           .b = 0.28466892,
                           .c = 0.55991073,
                           .r = 0.5};
double EvaluateHlgEotf(double e) {
  if (e < kSignalMin)
    return kSignalMin / kHlgScale;
  if (e <= kHlg.r)
    return pow(e / kHlg.r, 2.0) / kHlgScale;
  return (exp((e - kHlg.c) / kHlg.a) + kHlg.b) / kHlgScale;
}

uint32_t SignalToUint32(double signal) {
  signal = std::clamp(signal, kSignalMin, kSignalMax);
  signal = std::round(signal * static_cast<double>(UINT32_MAX));
  return static_cast<uint32_t>(signal);
}

uint16_t SignalToUint16(double signal) {
  signal = std::clamp(signal, kSignalMin, kSignalMax);
  signal = std::round(signal * static_cast<double>(UINT16_MAX));
  return static_cast<uint16_t>(signal);
}

template <typename T>
Lut1D<T> CreateLut(TransferFunction tf, uint32_t lut_size,
                   const double lut_scale, bool is_degamma) {
  const double scaled_step = 1.0 / (static_cast<double>(lut_size) - 1.0);
  Lut1D<T> lut(lut_size);
  for (size_t i = 0; i < lut_size; ++i) {
    double signal = static_cast<double>(i) * scaled_step;
    if (!is_degamma && NeedsTonemapping(tf)) {
      signal *= std::clamp(lut_scale, kSignalMin, kSignalMax);
    }

    switch (tf) {
      case TransferFunction::kPq:
        signal = is_degamma ? EvaluatePqEotf(signal) : EvaluatePqOetf(signal);
        break;
      case TransferFunction::kHlg:
        signal = is_degamma ? EvaluateHlgEotf(signal)
                            : ColorUtil::EvaluateHlgOetf(signal);
        break;
      case TransferFunction::kSrgb:
        signal = is_degamma ? ColorGamut::sRGB().toLinear(signal)[0]
                            : ColorGamut::sRGB().fromLinear(signal)[0];
        break;
      case TransferFunction::kSmpte170M:
        // ColorGamut::BT709 uses SMPTE 170M transfer parameters
        signal = is_degamma ? ColorGamut::BT709().toLinear(signal)[0]
                            : ColorGamut::BT709().fromLinear(signal)[0];
        break;
      case TransferFunction::kUnknown:
        [[fallthrough]];
      default:
        break;
    }

    if (is_degamma && NeedsTonemapping(tf)) {
      signal /= std::clamp(lut_scale, kSignalMin, kSignalMax);
    }
    if constexpr (std::is_same_v<T, drm_color_lut32>) {
      lut[i].red = lut[i].green = lut[i].blue = SignalToUint32(signal);
    } else {
      lut[i].red = lut[i].green = lut[i].blue = SignalToUint16(signal);
    }
    lut[i].reserved = 0;
  }

  return lut;
}
// NOLINTEND(readability-magic-numbers)

template <typename T>
const Lut1D<T> &Get1DLut(TransferFunction tf, const size_t lut_size,
                         Lut1DCache<T> &lut_1d_map, const float lut_scale,
                         bool is_degamma) {
  if (lut_size < 2) {
    ALOGE("Bad LUT size requested: %zu", lut_size);
    return kEmptyLut<T>;
  }

  auto key = std::tie(tf, lut_size, lut_scale);
  if (lut_1d_map.count(key) == 0) {
    lut_1d_map.emplace(key, CreateLut<T>(tf, lut_size, lut_scale, is_degamma));
  }
  return lut_1d_map.at(key);
}

}  // namespace

double ColorUtil::EvaluateHlgOetf(double l) {
  const double gamma_threshold = 1.0;
  l *= kHlgScale;
  if (l < kSignalMin)
    return kSignalMin;
  if (l <= gamma_threshold)
    return kHlg.r * sqrt(l);
  return (kHlg.a * log(l - kHlg.b)) + kHlg.c;
}

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
    const android::mat4d &color_transform_matrix) {
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

template <typename T>
std::shared_ptr<T> ColorUtil::GamutAdjustIfNeeded(
    Colorspace src_colorspace, Colorspace dest_colorspace,
    const std::shared_ptr<HalColorTransforMatrix> &color_transform_matrix,
    CscCache &color_transform_cache) {
  if (src_colorspace == dest_colorspace) {
    if constexpr (std::is_same_v<T, drm_color_ctm>) {
      return ColorUtil::ToColorTransform3x3(color_transform_matrix);
    } else if constexpr (std::is_same_v<T, drm_color_ctm_3x4>) {
      return ColorUtil::ToColorTransform3x4(color_transform_matrix);
    }
  }

  const HalColorTransforMatrix &ctm_in = color_transform_matrix
                                             ? *color_transform_matrix
                                             : kIdentityMatrix;
  // Extract the inner 3x3 matrix from the 4x4 CTM
  // NOLINTBEGIN(readability-magic-numbers)
  // clang-format off
  mat3d ctm3(
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

  if constexpr (std::is_same_v<T, drm_color_ctm>) {
    return android::drm_hwcomposer::ToColorTransform3x3(ctm3);
  } else if constexpr (std::is_same_v<T, drm_color_ctm_3x4>) {
    // Insert the new 3x3 matrix back into the 4x4 CTM
    // NOLINTBEGIN(readability-magic-numbers)
    // clang-format off
    mat4d ctm4 = mat4d(
      ctm3[0][0], ctm3[0][1], ctm3[0][2], ctm_in[3],
      ctm3[1][0], ctm3[1][1], ctm3[1][2], ctm_in[7],
      ctm3[2][0], ctm3[2][1], ctm3[2][2], ctm_in[11],
      ctm_in[12], ctm_in[13], ctm_in[14], ctm_in[15]
    );
    // clang-format on
    // NOLINTEND(readability-magic-numbers)

    return ToColorTransform3x4(ctm4);
  }
}

const Lut1D<drm_color_lut32> &ColorUtil::GetDegammaLut(
    TransferFunction dest_tf, TransferFunction src_tf, const size_t lut_size,
    Lut1DCache<drm_color_lut32> &lut_1d_map, const float layer_brightness) {
  if (!NeedsTonemapping(dest_tf)) {
    return kEmptyLut<drm_color_lut32>;
  }

  // Validate layer brightness
  auto lut_scale = (float)kSignalMax;
  if (layer_brightness > kSignalMin && layer_brightness < kSignalMax) {
    lut_scale = layer_brightness;
  }
  return Get1DLut<drm_color_lut32>(src_tf, lut_size, lut_1d_map, lut_scale,
                                   /*is_degamma=*/true);
}

const Lut1D<drm_color_lut> &ColorUtil::GetGammaLut(
    TransferFunction tf, const size_t lut_size,
    Lut1DCache<drm_color_lut> &lut_1d_map, const float display_brightness,
    const float hdr_headroom) {
  if (!NeedsTonemapping(tf)) {
    return kEmptyLut<drm_color_lut>;
  }

  // Validate display brightness
  auto lut_scale = (float)kSignalMax;
  if (display_brightness >= kSignalMin && display_brightness < kSignalMax) {
    lut_scale = display_brightness;
  }
  // Validate HDR headroom
  if (hdr_headroom > kSignalMin && hdr_headroom < kSignalMax) {
    lut_scale *= hdr_headroom;
  }
  return Get1DLut<drm_color_lut>(tf, lut_size, lut_1d_map, lut_scale,
                                 /*is_degamma=*/false);
}

// Tell the compiler explicitly to build these versions
template std::shared_ptr<drm_color_ctm> ColorUtil::GamutAdjustIfNeeded<
    drm_color_ctm>(Colorspace, Colorspace,
                   const std::shared_ptr<HalColorTransforMatrix> &, CscCache &);
template std::shared_ptr<drm_color_ctm_3x4>
ColorUtil::GamutAdjustIfNeeded<drm_color_ctm_3x4>(
    Colorspace, Colorspace, const std::shared_ptr<HalColorTransforMatrix> &,
    CscCache &);

}  // namespace android::drm_hwcomposer
