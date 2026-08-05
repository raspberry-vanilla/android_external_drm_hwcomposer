/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <array>
#include <cstdint>
#include <memory>

namespace android::drm_hwcomposer {

using DisplayHandle = int64_t;

enum class PowerMode {
  kOff,
  kDoze,
  kDozeSuspend,
  kSuspend,
  kOn,
};

constexpr int kColorMatrixSize = 16;
using HalColorTransformMatrix = std::array<float, kColorMatrixSize>;

/*
 * 4x4 Identity matrix used for color transformations.
 */
// clang-format off
// NOLINTNEXTLINE(clang-diagnostic-unused-const-variable)
constexpr HalColorTransformMatrix kIdentityMatrix = {
    1.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 0.0F, 1.0F,
};
// clang-format on

inline const std::shared_ptr<const HalColorTransformMatrix>&
GetIdentityCtmPtr() {
  static const auto
      kIdentityPtr = std::make_shared<const HalColorTransformMatrix>(
          kIdentityMatrix);
  return kIdentityPtr;
}

/*
 * 4x4 Boosted CTM for kVendorBoostedRenderIntent.
 *
 * WARNING: This matrix must not have an offset. Ensure that requirement is
 * satisfied if it is ever modified.
 */
// clang-format off
// NOLINTNEXTLINE(clang-diagnostic-unused-const-variable)
constexpr HalColorTransformMatrix kBoostedMatrix = {
    1.097F, -0.026F, -0.026F, 0.000F,
   -0.088F,  1.035F, -0.088F, 0.000F,
   -0.009F, -0.009F,  1.114F, 0.000F,
    0.000F,  0.000F,  0.000F, 1.000F,
};
// clang-format on

inline const std::shared_ptr<const HalColorTransformMatrix>&
GetBoostedCTMPtr() {
  static const auto
      kBoostedPtr = std::make_shared<const HalColorTransformMatrix>(
          kBoostedMatrix);
  return kBoostedPtr;
}

/*
 * Display colorimetry enums.
 */
// NOLINTBEGIN(readability-identifier-naming)
enum class ColorMode : int32_t {
  kNative,
  kBt601_625,
  kBt601_625Unadjusted,
  kBt601_525,
  kBt601_525Unadjusted,
  kBt709,
  kDciP3,
  kSrgb,
  kAdobeRgb,
  kDisplayP3,
  kBt2020,
  kBt2100Pq,
  kBt2100Hlg,
  kDisplayBt2020,
};
// NOLINTEND(readability-identifier-naming)

/**
 * Display panel colorspace operational values handled by drm_hwcomposer.
 */
enum class HwcColorspace : int32_t {
  kDefault,
  kBt601,
  kBt709,
  kDciP3,
  kBt2020,
};

/**
 * Display panel orientation property values.
 */
enum class PanelOrientation {
  kModePanelOrientationNormal = 0,
  kModePanelOrientationBottomUp,
  kModePanelOrientationLeftUp,
  kModePanelOrientationRightUp
};

/*
 * Content type to be used for HDMI infoframes.
 */
enum class ContentType { kNoData, kGraphics, kPhoto, kCinema, kGame };

struct QueuedConfigTiming {
  // In order for the new config to be applied, the client must send a new frame
  // at this time.
  int64_t refresh_time_ns;

  // The time when the display will start to refresh at the new vsync period.
  int64_t new_vsync_time_ns;
};

// Enum for HDCP Content Type
enum class HdcpContentType : int {
  kType0,  // This corresponds to DRM_MODE_HDCP_CONTENT_TYPE0
  kType1   // This corresponds to DRM_MODE_HDCP_CONTENT_TYPE1
};

// Enum for HDCP Status
enum class ContentProtection {
  kUndesired,  // This corresponds to DRM_MODE_CONTENT_PROTECTION_UNDESIRED
  kDesired,    // This corresponds to DRM_MODE_CONTENT_PROTECTION_DESIRED
  kEnabled,    // This corresponds to DRM_MODE_CONTENT_PROTECTION_ENABLED
};

}  // namespace android::drm_hwcomposer
