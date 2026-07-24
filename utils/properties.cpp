/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "properties.h"

#include <string>

#include "utils/log.h"

#ifdef ANDROID

#include <cutils/properties.h>

#else

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// NOLINTNEXTLINE(readability-identifier-naming)
constexpr int PROPERTY_VALUE_MAX = 92;

// NOLINTNEXTLINE(readability-identifier-naming)
auto inline property_get(const char *name, char *value,
                         const char *default_value) -> int {
  // NOLINTNEXTLINE (concurrency-mt-unsafe)
  char *prop = std::getenv(name);
  snprintf(value, PROPERTY_VALUE_MAX, "%s",
           (prop == nullptr) ? default_value : prop);
  return static_cast<int>(strlen(value));
}

/**
 * Bluntly copied from system/core/libcutils/properties.cpp,
 * which is part of the Android Project and licensed under Apache 2.
 * Source:
 * https://cs.android.com/android/platform/superproject/main/+/main:system/core/libcutils/properties.cpp;l=27
 */
auto inline property_get_bool(const char *key, int8_t default_value) -> int8_t {
  if (!key)
    return default_value;

  int8_t result = default_value;
  char buf[PROPERTY_VALUE_MAX] = {};

  int len = property_get(key, buf, "");
  if (len == 1) {
    char ch = buf[0];
    if (ch == '0' || ch == 'n') {
      result = false;
    } else if (ch == '1' || ch == 'y') {
      result = true;
    }
  } else if (len > 1) {
    if (!strcmp(buf, "no") || !strcmp(buf, "false") || !strcmp(buf, "off")) {
      result = false;
    } else if (!strcmp(buf, "yes") || !strcmp(buf, "true") ||
               !strcmp(buf, "on")) {
      result = true;
    }
  }

  return result;
}

/**
 * Bluntly copied from system/core/libcutils/properties.cpp,
 * which is part of the Android Project and licensed under Apache 2.
 * Source:
 * https://cs.android.com/android/platform/superproject/main/+/main:system/core/libcutils/properties.cpp;l=53
 */
auto inline property_get_int32(const char *key, int32_t default_value)
    -> int32_t {
  if (!key)
    return default_value;

  char value[PROPERTY_VALUE_MAX] = {};
  if (property_get(key, value, "") < 1)
    return default_value;

  // libcutils unwisely allows octal, which libbase doesn't.
  int32_t result = default_value;
  int saved_errno = errno;
  errno = 0;
  char *end = nullptr;
  intmax_t v = strtoimax(value, &end, 0);
  if (errno != ERANGE && end != value &&
      v >= std::numeric_limits<int32_t>::min() &&
      v <= std::numeric_limits<int32_t>::max()) {
    result = v;
  }
  errno = saved_errno;
  return result;
}

}  // namespace
#endif

namespace android::drm_hwcomposer {

/**
 * Force a color mode for the internal panel. See
 * android::drm_hwcomposer::ColorMode for valid values.
 */
auto Properties::ForceColorMode() -> int {
  return property_get_int32("vendor.hwc.drm.force_color_mode", -1);
}

/**
 * Determine if color pipeline feature is enabled. This uses the color pipeline
 * plane property to configure color settings instead of color_encoding etc.
 */
auto Properties::UseColorPipeline() -> bool {
  return (property_get_bool("vendor.hwc.drm.enable_color_pipeline", 0) != 0);
}

auto Properties::PersistentHdrEnabled() -> bool {
  return (property_get_bool("vendor.hwc.drm.persistent_hdr", 0) != 0);
}

/**
 * @brief Determine if the "Present Not Reliable" property is enabled.
 *
 * @return boolean
 */
auto Properties::IsPresentFenceNotReliable() -> bool {
  return (property_get_bool("ro.vendor.hwc.drm.present_fence_not_reliable",
                            0) != 0);
}

auto Properties::InternalDisplayNames() -> std::string {
  char buf[PROPERTY_VALUE_MAX] = {};
  property_get("vendor.hwc.drm.internal_display_names", buf, "");
  return {buf};
}

auto Properties::UseOverlayPlanes() -> bool {
  return (property_get_bool("ro.vendor.hwc.use_overlay_planes", 1) != 0);
}

auto Properties::ScaleWithGpu() -> bool {
  return (property_get_bool("vendor.hwc.drm.scale_with_gpu", 0) != 0);
}

auto Properties::EnableVirtualDisplay() -> bool {
  return (property_get_bool("vendor.hwc.drm.enable_virtual_display", 0) != 0);
}

auto Properties::EnableExternalDisplays() -> bool {
  return (property_get_bool("vendor.hwc.drm.enable_external_displays", 1) != 0);
}

auto Properties::ForcedHolePunchingEnabled() -> bool {
  return (property_get_bool("ro.surface_flinger.force_hole_punch", 0) != 0);
}

auto Properties::GetCtmHandling() -> CtmHandling {
  char proptext[PROPERTY_VALUE_MAX];
  constexpr char kDrmOrGpu[] = "DRM_OR_GPU";
  constexpr char kDrmOrIgnore[] = "DRM_OR_IGNORE";
  property_get("vendor.hwc.drm.ctm", proptext, "");
  if (strncmp(proptext, kDrmOrGpu, sizeof(kDrmOrGpu)) == 0) {
    return CtmHandling::kDrmOrGpu;
  }
  if (strncmp(proptext, kDrmOrIgnore, sizeof(kDrmOrIgnore)) == 0) {
    return CtmHandling::kDrmOrIgnore;
  }

  ALOGE_IF(proptext[0] != '\0', "Invalid value for vendor.hwc.drm.ctm: %s",
           proptext);
  // Default value.
  return CtmHandling::kDrmOrGpu;
}

auto Properties::BugfixCursorCtmOffset() -> bool {
  constexpr int kDefault = 1;
  return (property_get_bool("vendor.hwc.drm.bugfix_cursor_ctm_offset",
                            kDefault) != 0);
}

auto Properties::GetBackendOverride() -> std::string {
  char backend_override[PROPERTY_VALUE_MAX];
  property_get("vendor.hwc.backend_override", backend_override, "");
  return {backend_override};
}

auto Properties::GetDevicePath() -> std::string {
  char path_pattern[PROPERTY_VALUE_MAX];
  // Could be a valid path or it can have at the end of it the wildcard %
  // which means that it will try open all devices until an error is met.
  property_get("vendor.hwc.drm.device", path_pattern, "");
  return {path_pattern};
}

auto Properties::GetForceMode() -> std::string {
  char force_mode[PROPERTY_VALUE_MAX];
  property_get("vendor.hwc.drm.force_mode", force_mode, "");
  return {force_mode};
}

auto Properties::ValidationShortCircuiting() -> bool {
  constexpr int kDefault = 1;
  return (property_get_bool("vendor.hwc.drm.validation_short_circuiting",
                            kDefault) != 0);
}

auto Properties::ShortCircuitIgnoreGeometry() -> bool {
  constexpr int kDefault = 0;
  return (property_get_bool("vendor.hwc.drm.short_circuit_ignore_geometry",
                            kDefault) != 0);
}

auto Properties::ShortCircuitIgnoreCtm() -> bool {
  constexpr int kDefault = 0;
  return (property_get_bool("vendor.hwc.drm.short_circuit_ignore_ctm",
                            kDefault) != 0);
}

}  // namespace android::drm_hwcomposer
