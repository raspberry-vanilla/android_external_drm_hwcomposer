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

namespace android::drm_hwcomposer {

/*
 * 4x4 Identity matrix used for color transformations.
 */
constexpr int kColorMatrixSize = 16;
// clang-format off
// NOLINTNEXTLINE(clang-diagnostic-unused-const-variable)
constexpr std::array<float, kColorMatrixSize> kIdentityMatrix = {
    1.0F, 0.0F, 0.0F, 0.0F,
    0.0F, 1.0F, 0.0F, 0.0F,
    0.0F, 0.0F, 1.0F, 0.0F,
    0.0F, 0.0F, 0.0F, 1.0F,
};
// clang-format on

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
 * Display panel colorspace property values.
 */
enum class Colorspace : int32_t {
  kDefault,
  kSmpte170MYcc,
  kBt709Ycc,
  kXvycc601,
  kXvycc709,
  kSycc601,
  kOpycc601,
  kOprgb,
  kBt2020Cycc,
  kBt2020Rgb,
  kBt2020Ycc,
  kDciP3RgbD65,
  kDciP3RgbTheater,
  kRgbWideFixed,
  kRgbWideFloat,
  kBt601Ycc,
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
