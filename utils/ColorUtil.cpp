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

#include <memory>
#include <tuple>

#include <ui/ColorSpace.h>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "math/TMatHelpers.h"

using ColorGamut = android::ColorSpace;

namespace android::drm_hwcomposer {

namespace {

// TODO: use layer data and display luminance to set this value
constexpr float kHdrHeadroom = 1000.F / 10000.F;

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
      return true;
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
  float n, m, c1, c2, c3;
};
const PqConstants kPq = {.n = 0.1593017578125F,
                         .m = 78.84375F,
                         .c1 = 0.8359375F,
                         .c2 = 18.83203125F,
                         .c3 = 18.68359375F};
// NOLINTBEGIN(readability-magic-numbers)
float EvaluatePqOetf(float l) {
  l *= kHdrHeadroom;
  if (l <= 0.F)
    return 0.F;
  float l1 = powf(l, kPq.n);
  float base = (kPq.c1 + (kPq.c2 * l1)) / (1.F + (kPq.c3 * l1));
  return powf(base, kPq.m);
}
float EvaluatePqEotf(float e) {
  if (e <= 0.F)
    return 0.F;
  float em = powf(e, 1.F / kPq.m);
  float base = (fmaxf(em - kPq.c1, 0.F)) / (kPq.c2 - (kPq.c3 * em));
  return powf(base, 1.0F / kPq.n) / kHdrHeadroom;
}

/**
 * Keep in sync with the Dataspace.aidl SRGB definition
 * Transfer characteristic curve:
 *
 * E = 1.055 * L^(1/2.4) - 0.055  for 0.0031308 <= L <= 1
 *   = 12.92 * L                  for 0 <= L < 0.0031308
 *     L - luminance of image 0 <= L <= 1 for conventional colorimetry
 *     E - corresponding electrical signal
 * https://cs.android.com/android/platform/superproject/main/+/main:hardware/interfaces/graphics/common/aidl/android/hardware/graphics/common/Dataspace.aidl;l=275;drc=dbf753b896a75f3e712bc362a01763d731e49f57
 */
struct TfConstants {
  float g, a, b, c, d, e, f;
};
float EvaluateGamma(float e, const TfConstants fn) {
  if (e < fn.d)
    return (e * fn.c) + fn.f;
  return powf((e + fn.b) * fn.a, fn.g) + fn.e;
}
TfConstants GetInverse(const TfConstants fn) noexcept {
  TfConstants inv{};
  if (fn.a > 0.F && fn.g > 0.F) {
    float a_to_the_g = pow(fn.a, fn.g);
    inv.a = 1.F / a_to_the_g;
    inv.b = -fn.e / a_to_the_g;
    inv.g = 1.F / fn.g;
  }
  inv.d = fn.c * fn.d + fn.f;
  inv.e = -fn.b / fn.a;
  if (fn.c != 0.F) {
    inv.c = 1.F / fn.c;
    inv.f = -fn.f / fn.c;
  }
  return inv;
}
const TfConstants kSrgb = {.g = 2.4F,
                           .a = (1.F / 1.055F),
                           .b = (0.055F / 1.055F),
                           .c = (1.F / 12.92F),
                           .d = (12.92F * 0.0031308F),
                           .e = 0.F,
                           .f = 0.F};
const TfConstants kInverseSrgb = GetInverse(kSrgb);

uint32_t SignalToInt(float signal) {
  signal = std::clamp(signal, 0.F, 1.F);
  signal = std::round(signal * static_cast<float>(UINT32_MAX));
  return static_cast<uint32_t>(signal);
}

Lut1D CreateLut(TransferFunction tf, uint32_t lut_size, bool is_degamma) {
  std::vector<drm_color_lut32> lut(lut_size);
  for (size_t i = 0; i < lut_size; ++i) {
    float signal = static_cast<float>(i) / (static_cast<float>(lut_size) - 1.F);
    switch (tf) {
      case TransferFunction::kPq:
        signal = is_degamma ? EvaluatePqEotf(signal) : EvaluatePqOetf(signal);
        break;
      case TransferFunction::kSrgb:
        signal = is_degamma ? EvaluateGamma(signal, kSrgb)
                            : EvaluateGamma(signal, kInverseSrgb);
        break;
      default:
        break;
    }

    lut[i].red = lut[i].green = lut[i].blue = SignalToInt(signal);
    lut[i].reserved = 0;
  }

  return lut;
}
// NOLINTEND(readability-magic-numbers)

const Lut1D &Get1DLut(
    TransferFunction tf, const size_t lut_size,
    std::map<std::tuple<TransferFunction, size_t>, Lut1D> &lut_1d_map,
    bool is_degamma) {
  if (lut_size < 2) {
    ALOGE("Bad LUT size requested: %zu", lut_size);
    return kEmptyLut;
  }

  auto key = std::tie(tf, lut_size);
  if (lut_1d_map.count(key) == 0) {
    lut_1d_map.emplace(key, CreateLut(tf, lut_size, is_degamma));
  }
  return lut_1d_map.at(key);
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

std::tuple<const Lut1D &, const Lut1D &> ColorUtil::Get1DLutsIfNeeded(
    TransferFunction src_tf, TransferFunction dest_tf,
    const size_t degamma_lut_size, const size_t gamma_lut_size,
    std::map<std::tuple<TransferFunction, size_t>, Lut1D> &degamma_lut_map,
    std::map<std::tuple<TransferFunction, size_t>, Lut1D> &gamma_lut_map) {
  if (src_tf == dest_tf) {
    return std::tie(kEmptyLut, kEmptyLut);
  }

  if (NeedsTonemapping(src_tf) || NeedsTonemapping(dest_tf)) {
    return {Get1DLut(src_tf, degamma_lut_size, degamma_lut_map,
                     /*is_degamma=*/true),
            Get1DLut(dest_tf, gamma_lut_size, gamma_lut_map,
                     /*is_degamma=*/false)};
  }

  return std::tie(kEmptyLut, kEmptyLut);
}

}  // namespace android::drm_hwcomposer
