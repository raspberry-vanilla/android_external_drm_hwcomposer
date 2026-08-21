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
#include <math/vec3.h>
#include <ui/ColorSpace.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "utils/log.h"
#include "utils/math.h"
#include "utils/properties.h"

using ColorGamut = android::ColorSpace;

constexpr int kOffsetRedIndex = 12;
constexpr int kOffsetGreenIndex = 13;
constexpr int kOffsetBlueIndex = 14;

namespace android::drm_hwcomposer {

namespace {

// Normalize to the range [0, 12] rather than [0, 1]
const double kHlgScale = 12.0;

template <typename T>
std::shared_ptr<T> ToColorTransform(
    const std::shared_ptr<const HalColorTransformMatrix>
        &color_transform_matrix,
    const bool output_is_3x4_matrix) {
  if (!color_transform_matrix)
    return nullptr;

  std::shared_ptr<T> color_matrix = std::make_shared<T>();
  const int rows = output_is_3x4_matrix ? 4 : 3;
  constexpr int kCols = 3;
  constexpr int kHalRows = 4;
  for (int i = 0; i < kCols; i++) {
    for (int j = 0; j < rows; j++) {
      color_matrix->matrix[(i * rows) + j] = ColorUtil::To3132FixPt(
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
      color_matrix->matrix[(i * kDim) + j] = ColorUtil::To3132FixPt(
          color_transform_matrix[j][i]);
    }
  }
  return color_matrix;
}

mat3d GetGamutTransform(HwcColorspace src, HwcColorspace dest) {
  return mat3d(ColorSpaceConnector(ColorUtil::ToColorGamut(src),
                                   ColorUtil::ToColorGamut(dest))
                   .getTransform());
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
  if (lut_size < 2) {
    ALOGE("Bad LUT size requested: %u", lut_size);
    return {};
  }
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
      case TransferFunction::kUnknown:
        ALOGV("Unknown transfer function, falling back to sRGB");
        [[fallthrough]];
      case TransferFunction::kSrgb:
        signal = is_degamma ? kSrgbGamut.toLinear(signal)[0]
                            : kSrgbGamut.fromLinear(signal)[0];
        break;
      case TransferFunction::kSmpte170M:
        // BT.709 uses SMPTE 170M transfer parameters
        signal = is_degamma ? kBt709Gamut.toLinear(signal)[0]
                            : kBt709Gamut.fromLinear(signal)[0];
        break;
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

// Converts a double into DRM fixed point format (S31.32 sign-magnitude):
// Bit 63: Sign bit (0 for positive, 1 for negative)
// Bits 62-32: 31-bit integer magnitude
// Bits 31-0: 32-bit fractional magnitude (1.0 == (1ULL << 32))
uint64_t ColorUtil::To3132FixPt(double in) {
  if (std::isnan(in)) {
    return 0;
  }

  constexpr uint64_t kSignBit = 1ULL << 63;
  constexpr uint64_t kValueMask = (1ULL << 63) - 1;
  constexpr auto kFractionalScale = static_cast<double>(1ULL << 32);

  const bool is_negative = std::signbit(in);
  const double abs_in = std::abs(in);

  const double scaled = std::round(abs_in * kFractionalScale);

  uint64_t val = 0;
  if (scaled >= static_cast<double>(kValueMask)) {
    val = kValueMask;
  } else {
    val = static_cast<uint64_t>(scaled);
  }

  return is_negative ? (kSignBit | val) : val;
}

std::shared_ptr<drm_color_ctm> ColorUtil::ToColorTransform3x3(
    const std::shared_ptr<const HalColorTransformMatrix>
        &color_transform_matrix) {
  return ToColorTransform<drm_color_ctm>(color_transform_matrix,
                                         /*output_is_3x4_matrix=*/false);
}

std::shared_ptr<drm_color_ctm_3x4> ColorUtil::ToColorTransform3x4(
    const std::shared_ptr<const HalColorTransformMatrix>
        &color_transform_matrix) {
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
      color_matrix->matrix[(i * kRows) + j] = ColorUtil::To3132FixPt(
          color_transform_matrix[j][i]);
    }
  }
  return color_matrix;
}

bool ColorUtil::HasOffset(const HalColorTransformMatrix &matrix) {
  return !FloatEquals(matrix[kOffsetRedIndex], 0.F) ||
         !FloatEquals(matrix[kOffsetGreenIndex], 0.F) ||
         !FloatEquals(matrix[kOffsetBlueIndex], 0.F);
}

std::shared_ptr<std::array<uint64_t, 3>> ColorUtil::ToColorOffset(
    const std::shared_ptr<const HalColorTransformMatrix>
        &color_transform_matrix) {
  if (!color_transform_matrix)
    return nullptr;

  auto offsets = std::make_shared<std::array<uint64_t, 3>>();
  (*offsets)[0] = To3132FixPt((*color_transform_matrix)[kOffsetRedIndex]);
  (*offsets)[1] = To3132FixPt((*color_transform_matrix)[kOffsetGreenIndex]);
  (*offsets)[2] = To3132FixPt((*color_transform_matrix)[kOffsetBlueIndex]);
  return offsets;
}

std::shared_ptr<std::array<uint64_t, 3>> ColorUtil::ToColorOffset(
    HwcColorspace src_colorspace, HwcColorspace dest_colorspace,
    const std::shared_ptr<const HalColorTransformMatrix>
        &color_transform_matrix) {
  if (!color_transform_matrix)
    return nullptr;

  if (src_colorspace == dest_colorspace) {
    return ToColorOffset(color_transform_matrix);
  }

  const HalColorTransformMatrix &ctm_in = *color_transform_matrix;
  mat3d gamut_transform = GetGamutTransform(src_colorspace, dest_colorspace);
  double3 offset = gamut_transform * double3(ctm_in[kOffsetRedIndex],
                                             ctm_in[kOffsetGreenIndex],
                                             ctm_in[kOffsetBlueIndex]);

  auto offsets = std::make_shared<std::array<uint64_t, 3>>();
  (*offsets)[0] = To3132FixPt(offset[0]);
  (*offsets)[1] = To3132FixPt(offset[1]);
  (*offsets)[2] = To3132FixPt(offset[2]);
  return offsets;
}

std::shared_ptr<const HalColorTransformMatrix> ColorUtil::Multiply(
    const std::shared_ptr<const HalColorTransformMatrix> &a,
    const std::shared_ptr<const HalColorTransformMatrix> &b) {
  if (a == nullptr && b == nullptr) {
    return nullptr;
  }
  if (a == nullptr || a == GetIdentityCtmPtr()) {
    return b;
  }
  if (b == nullptr || b == GetIdentityCtmPtr()) {
    return a;
  }

  android::mat4 mat_a(static_cast<const float *>(a->data()));
  android::mat4 mat_b(static_cast<const float *>(b->data()));
  android::mat4 res = mat_a * mat_b;
  auto out = std::make_shared<HalColorTransformMatrix>();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::copy(res.asArray(), res.asArray() + kColorMatrixSize, out->begin());
  return out;
}

const ColorGamut &ColorUtil::ToColorGamut(HwcColorspace colorspace) {
  switch (colorspace) {
    case HwcColorspace::kBt709:
    case HwcColorspace::kDefault:
      return kBt709Gamut;
    case HwcColorspace::kBt2020:
      return kBt2020Gamut;
    case HwcColorspace::kDciP3:
      return kDciP3Gamut;
    case HwcColorspace::kBt601:
      return kSrgbGamut;
    default:
      ALOGW("Unknown colorspace %d, falling back to sRGB", colorspace);
      return kSrgbGamut;
  }
}

// Maps framework ColorMode to its corresponding EOTF transfer function curve.
const ColorGamut::transfer_function &ColorUtil::GetEotf(ColorMode mode) {
  switch (mode) {
    case ColorMode::kSrgb:
    case ColorMode::kBt601_625:
    case ColorMode::kBt601_625Unadjusted:
    case ColorMode::kBt601_525:
    case ColorMode::kBt601_525Unadjusted:
      return kSrgbGamut.getEOTF();
    case ColorMode::kBt709:
      return kBt709Gamut.getEOTF();
    case ColorMode::kDciP3:
    case ColorMode::kDisplayP3:
      return kDciP3Gamut.getEOTF();
    case ColorMode::kBt2020:
    case ColorMode::kDisplayBt2020:
      return kBt2020Gamut.getEOTF();
    default:
      return kSrgbGamut.getEOTF();
  }
}

HalColorTransformMatrix ColorUtil::ToLinearCtm(
    const HalColorTransformMatrix ctm_in, ColorMode mode) {
  // EOTF element-wise conversion is only valid for non-negative diagonal
  // scaling matrices (such as Night Light). For CTMs with offsets,
  // negative values, or cross-talk (e.g. Color Inversion), return the
  // matrix untouched.
  const bool is_diagonal = FloatEquals(ctm_in[1], 0.F) &&
                           FloatEquals(ctm_in[2], 0.F) &&
                           FloatEquals(ctm_in[3], 0.F) &&
                           FloatEquals(ctm_in[4], 0.F) &&
                           FloatEquals(ctm_in[6], 0.F) &&
                           FloatEquals(ctm_in[7], 0.F) &&
                           FloatEquals(ctm_in[8], 0.F) &&
                           FloatEquals(ctm_in[9], 0.F) &&
                           FloatEquals(ctm_in[11], 0.F) &&
                           FloatEquals(ctm_in[12], 0.F) &&
                           FloatEquals(ctm_in[13], 0.F) &&
                           FloatEquals(ctm_in[14], 0.F);
  const bool has_negative = std::any_of(ctm_in.begin(), ctm_in.end(),
                                        [](float val) { return val < 0.F; });

  if (!is_diagonal || has_negative) {
    return ctm_in;
  }

  HalColorTransformMatrix ctm_out = kIdentityMatrix;
  const ColorGamut::transfer_function &tf = GetEotf(mode);
  std::transform(ctm_in.begin(), ctm_in.end(), ctm_out.begin(),
                 [&tf](float val) { return tf(val); });
  return ctm_out;
}

template <typename T>
std::shared_ptr<T> ColorUtil::GamutAdjustIfNeeded(
    HwcColorspace src_colorspace, HwcColorspace dest_colorspace,
    const std::shared_ptr<const HalColorTransformMatrix>
        &color_transform_matrix) {
  if (src_colorspace == dest_colorspace) {
    if constexpr (std::is_same_v<T, drm_color_ctm>) {
      return ColorUtil::ToColorTransform3x3(color_transform_matrix);
    } else if constexpr (std::is_same_v<T, drm_color_ctm_3x4>) {
      return ColorUtil::ToColorTransform3x4(color_transform_matrix);
    }
  }

  const HalColorTransformMatrix &ctm_in = color_transform_matrix
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
  mat3d gamut_transform = GetGamutTransform(src_colorspace, dest_colorspace);
  ctm3 = gamut_transform * ctm3;

  if constexpr (std::is_same_v<T, drm_color_ctm>) {
    return android::drm_hwcomposer::ToColorTransform3x3(ctm3);
  } else if constexpr (std::is_same_v<T, drm_color_ctm_3x4>) {
    // Transform 3x4 offset translation vector into destination gamut (O_dest =
    // G * O_src)
    double3 offset = gamut_transform * double3(ctm_in[kOffsetRedIndex],
                                               ctm_in[kOffsetGreenIndex],
                                               ctm_in[kOffsetBlueIndex]);

    // Insert the new 3x3 matrix back into the 4x4 CTM
    // NOLINTBEGIN(readability-magic-numbers)
    // clang-format off
    mat4d ctm4 = mat4d(
      ctm3[0][0], ctm3[0][1], ctm3[0][2], ctm_in[3],
      ctm3[1][0], ctm3[1][1], ctm3[1][2], ctm_in[7],
      ctm3[2][0], ctm3[2][1], ctm3[2][2], ctm_in[11],
      offset[0], offset[1], offset[2], ctm_in[15]
    );
    // clang-format on
    // NOLINTEND(readability-magic-numbers)

    return ToColorTransform3x4(ctm4);
  }
}

Lut1D<drm_color_lut32> ColorUtil::CreateDegammaLut(
    TransferFunction tf, const size_t lut_size,
    const std::optional<float> layer_brightness) {
  float lut_scale = CalculateDegammaScale(layer_brightness);
  return CreateLut<drm_color_lut32>(tf, lut_size, lut_scale,
                                    /*is_degamma=*/true);
}

Lut1D<drm_color_lut> ColorUtil::CreateGammaLut(
    TransferFunction tf, const size_t lut_size,
    const std::optional<float> display_brightness,
    const std::optional<float> hdr_headroom) {
  float lut_scale = CalculateGammaScale(display_brightness, hdr_headroom);
  return CreateLut<drm_color_lut>(tf, lut_size, lut_scale,
                                  /*is_degamma=*/false);
}

auto ColorUtil::ScaleBrightnessIfNeeded(float display_brightness) -> float {
  // Pass through unset or out-of-range sentinel values untouched.
  if (display_brightness < kMinBrightness ||
      display_brightness > kMaxBrightness) {
    return display_brightness;
  }

  const float min_brightness = Properties::MinDisplayBrightness();
  if (min_brightness > kMinBrightness &&
      Properties::ScaleBrightnessRangeToMinBrightness()) {
    return min_brightness +
           (display_brightness * (kMaxBrightness - min_brightness));
  }

  return std::max(display_brightness, min_brightness);
}

// Tell the compiler explicitly to build these versions
template std::shared_ptr<drm_color_ctm> ColorUtil::GamutAdjustIfNeeded<
    drm_color_ctm>(HwcColorspace, HwcColorspace,
                   const std::shared_ptr<const HalColorTransformMatrix> &);
template std::shared_ptr<drm_color_ctm_3x4> ColorUtil::GamutAdjustIfNeeded<
    drm_color_ctm_3x4>(HwcColorspace, HwcColorspace,
                       const std::shared_ptr<const HalColorTransformMatrix> &);

}  // namespace android::drm_hwcomposer
