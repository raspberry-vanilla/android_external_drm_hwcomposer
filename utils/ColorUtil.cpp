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
#include <vector>

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

double EvaluateOetf(TransferFunction tf, double signal) {
  switch (tf) {
    case TransferFunction::kPq:
      return EvaluatePqOetf(signal);
    case TransferFunction::kHlg:
      return ColorUtil::EvaluateHlgOetf(signal);
    case TransferFunction::kUnknown:
      ALOGV("Unknown transfer function, falling back to sRGB");
      [[fallthrough]];
    case TransferFunction::kSrgb:
      return kSrgbGamut.fromLinear(signal)[0];
    case TransferFunction::kSmpte170M:
      return kBt709Gamut.fromLinear(signal)[0];
    default:
      return signal;
  }
}

double EvaluateEotf(TransferFunction tf, double signal) {
  switch (tf) {
    case TransferFunction::kPq:
      return EvaluatePqEotf(signal);
    case TransferFunction::kHlg:
      return EvaluateHlgEotf(signal);
    case TransferFunction::kUnknown:
      ALOGV("Unknown transfer function, falling back to sRGB");
      [[fallthrough]];
    case TransferFunction::kSrgb:
      return kSrgbGamut.toLinear(signal)[0];
    case TransferFunction::kSmpte170M:
      return kBt709Gamut.toLinear(signal)[0];
    default:
      return signal;
  }
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
    if (!is_degamma && ColorUtil::NeedsTonemapping(tf)) {
      signal *= std::clamp(lut_scale, kSignalMin, kSignalMax);
    }

    signal = is_degamma ? EvaluateEotf(tf, signal) : EvaluateOetf(tf, signal);

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

bool ColorUtil::NeedsTonemapping(TransferFunction tf) {
  switch (tf) {
    case TransferFunction::kPq:
    case TransferFunction::kHlg:
      return true;
    case TransferFunction::kSmpte170M:
    case TransferFunction::kSrgb:
    case TransferFunction::kUnknown:
    default:
      return false;
  }
}

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
    // NOLINTNEXTLINE(readability-magic-numbers)
    double3 alpha(ctm_in[3], ctm_in[7], ctm_in[11]);

    // Insert the new 3x3 matrix back into the 4x4 CTM
    // NOLINTBEGIN(readability-magic-numbers)
    // clang-format off
    mat4d ctm4 = mat4d(
      ctm3[0][0], ctm3[0][1], ctm3[0][2], alpha[0],
      ctm3[1][0], ctm3[1][1], ctm3[1][2], alpha[1],
      ctm3[2][0], ctm3[2][1], ctm3[2][2], alpha[2],
      offset[0],  offset[1],  offset[2],  ctm_in[15]
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

// NOLINTBEGIN(readability-magic-numbers)
std::vector<drm_color_lut32> ColorUtil::GetLogGammaLut(
    TransferFunction tf, const std::optional<float> display_brightness,
    const std::optional<float> hdr_headroom) {
  std::vector<drm_color_lut32> lut;

  // Number of entries in the multi-seg LUT: 510 standard + 3 boost/max
  // registers
  constexpr size_t kLogGammaLutEntries = 513;
  lut.resize(kLogGammaLutEntries);

  // Standard 1D LUT entries (0..509) in Intel logarithmic gamma mode use 16-bit
  // unsigned integer values [0, 65535] (0xFFFF), matching standard DRM LUT
  // channels.
  constexpr double kMaxStd = 65535.0;

  // The logarithmic gamma hardware domain is 23-bit fixed point (U0.23), where
  // full scale 1.0 corresponds to 2^23 (8,388,608) steps. Each octave segment
  // is specified in fixed point coordinates and normalized by dividing by 2^23.
  const auto total_steps = static_cast<double>(1 << 23);

  auto set_lut_entry = [&](size_t idx, double in_signal) {
    double out_signal = EvaluateOetf(tf, in_signal);
    uint32_t val = static_cast<uint32_t>(
        std::clamp(std::round(out_signal * kMaxStd), 0.0, kMaxStd));
    lut[idx].red = val;
    lut[idx].green = val;
    lut[idx].blue = val;
    lut[idx].reserved = 0;
  };

  float lut_scale = CalculateGammaScale(display_brightness, hdr_headroom);

  // First two discrete points: 0.0 and 1 / 2^24
  set_lut_entry(0, 0.0);
  set_lut_entry(1, 1.0 / total_steps * lut_scale);

  // Each segment represents one power-of-two octave in the logarithmic gamma
  // LUT. Darker regions have higher sample density to provide better resolution
  // where contrast is most critical. Matches d13_logarithmic_gamma hardware
  // specification.
  struct LogSegment {
    int offset;
    int samples;
  };

  // clang-format off
  constexpr LogSegment kLogSegments[] = {
    {0, 2},   {1, 2},   {2, 2}, {3, 2},
    {4, 4},   {5, 4},   {6, 4},
    {7, 8},   {8, 8},   {9, 8},
    {10, 16}, {11, 16}, {12, 16},
    {13, 32}, {14, 32},
    {15, 64}, {16, 64}, {17, 64},
    {18, 32}, {19, 32}, {20, 32},
    {21, 32}, {22, 32}};
  // clang-format on

  size_t out_idx = 2;
  for (const auto &seg : kLogSegments) {
    auto start_pos = static_cast<double>(1 << seg.offset);
    auto end_pos = static_cast<double>(1 << (seg.offset + 1));
    double step = (end_pos - start_pos) / static_cast<double>(seg.samples);

    for (int i = 0; i < seg.samples; ++i) {
      double signal = (start_pos + ((i + 1) * step)) / total_steps;
      set_lut_entry(out_idx, signal * lut_scale);
      ++out_idx;
    }
  }

  // Extended Elements for hardware boost / endpoint registers:
  // - lut[510] -> PREC_PAL_GC_MAX: u1.16 fixed-point format (scale 65536.0,
  // input X = 1.0)
  // - lut[511] -> PREC_PAL_EXT_GC_MAX: u3.16 fixed-point format (scale 65536.0,
  // input X = 3.0)
  // - lut[512] -> PREC_PAL_EXT2_GC_MAX: u3.16 fixed-point format (scale
  // 65536.0, input X = 7.0)
  constexpr double kFixedPoint16Scale = 65536.0;
  constexpr auto kGcMax = static_cast<double>((1 << 17) - 1);     // 0x1FFFF
  constexpr auto kExtGcMax = static_cast<double>((1 << 19) - 1);  // 0x7FFFF

  // 1. PREC_PAL_GC_MAX (u1.16) at X = 1.0
  double sig_1_0 = EvaluateOetf(tf, 1.0 * lut_scale);
  uint32_t gc_max = static_cast<uint32_t>(
      std::clamp(std::round(sig_1_0 * kFixedPoint16Scale), 0.0, kGcMax));
  lut[510].red = gc_max;
  lut[510].green = gc_max;
  lut[510].blue = gc_max;
  lut[510].reserved = 0;

  // 2. PREC_PAL_EXT_GC_MAX (u3.16) at X = 3.0
  double sig_3_0 = EvaluateOetf(tf, 3.0 * lut_scale);
  uint32_t ext_gc_max = static_cast<uint32_t>(
      std::clamp(std::round(sig_3_0 * kFixedPoint16Scale), 0.0, kExtGcMax));
  lut[511].red = ext_gc_max;
  lut[511].green = ext_gc_max;
  lut[511].blue = ext_gc_max;
  lut[511].reserved = 0;

  // 3. PREC_PAL_EXT2_GC_MAX (u3.16) at X = 7.0
  double sig_7_0 = EvaluateOetf(tf, 7.0 * lut_scale);
  uint32_t ext2_gc_max = static_cast<uint32_t>(
      std::clamp(std::round(sig_7_0 * kFixedPoint16Scale), 0.0, kExtGcMax));
  lut[512].red = ext2_gc_max;
  lut[512].green = ext2_gc_max;
  lut[512].blue = ext2_gc_max;
  lut[512].reserved = 0;

  return lut;
}
// NOLINTEND(readability-magic-numbers)

// Tell the compiler explicitly to build these versions
template std::shared_ptr<drm_color_ctm> ColorUtil::GamutAdjustIfNeeded<
    drm_color_ctm>(HwcColorspace, HwcColorspace,
                   const std::shared_ptr<const HalColorTransformMatrix> &);
template std::shared_ptr<drm_color_ctm_3x4> ColorUtil::GamutAdjustIfNeeded<
    drm_color_ctm_3x4>(HwcColorspace, HwcColorspace,
                       const std::shared_ptr<const HalColorTransformMatrix> &);

}  // namespace android::drm_hwcomposer
