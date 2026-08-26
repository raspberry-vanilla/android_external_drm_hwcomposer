/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <optional>
#include <string>

namespace android::drm_hwcomposer {

enum class CtmHandling {
  kDrmOrGpu,    /* Handled by DRM is possible, otherwise by GPU */
  kDrmOrIgnore, /* Handled by DRM is possible, otherwise displayed as is */
};

class Properties {
 public:
  static auto IsPresentFenceNotReliable() -> bool;
  static auto InternalDisplayNames() -> std::string;
  static auto UseOverlayPlanes() -> bool;
  static auto ScaleWithGpu() -> bool;
  static auto EnableVirtualDisplay() -> bool;
  static auto EnableExternalDisplays() -> bool;
  static auto EnableHdcpOnHotplug() -> bool;
  static auto GetCtmHandling() -> CtmHandling;
  static auto BugfixCursorCtmOffset() -> bool;
  static auto GetBackendOverride() -> std::string;
  static auto GetDevicePath() -> std::string;
  static auto GetForceMode() -> std::string;
  static auto UseColorPipeline() -> bool;
  static auto ForcedHolePunchingEnabled() -> bool;
  static auto ForceDisableMrr() -> bool;
  static auto MinRefreshRate() -> std::optional<int>;
  static auto MaxRefreshRate() -> std::optional<int>;
  static auto SkipInternalDisplayReset() -> bool;
  static auto ForceColorMode() -> int;
  static auto PersistentHdrEnabled() -> bool;
  static auto ValidationShortCircuiting() -> bool;
  static auto ShortCircuitIgnoreGeometry() -> bool;
  static auto ShortCircuitIgnoreCtm() -> bool;
  static auto ExternalHdrEnabled() -> bool;
  static auto SkipPlaneDamageClips() -> bool;
  static auto FlatteningEnabled() -> bool;
  static auto UseLogGammaLut() -> bool;

  /**
   * Minimum display brightness floor in the range [0.0, 1.0].
   * Default: 0.0.
   */
  static auto MinDisplayBrightness() -> float;

  /**
   * When enabled and min_display_brightness > 0.0, linearly scales the
   * brightness range into [min_display_brightness, 1.0] instead of clamping.
   */
  static auto ScaleBrightnessRangeToMinBrightness() -> bool;

  /**
   * Retrieves the filesystem path to the early boot animation package file
   * from the ro.vendor.hwc.bootanim.path system property.
   *
   * Default: "/vendor/etc/bootanim.raw"
   */
  static auto BootAnimationPath() -> std::string;

  /**
   * Signals whether the early boot animation has completed playback and
   * hold by setting the vendor.hwc.bootanim.completed system property.
   */
  static void SetBootAnimationCompleted(bool completed);

  /**
   * Retrieves the physical hardware hold time in milliseconds for the first
   * frame of the early boot animation via debug.hwc.early_boot_hold_ms.
   */
  static auto EarlyBootHoldMs() -> int;

  /**
   * Determines whether the early boot animation is enabled via the
   * vendor.hwc.drm.bootanim.enable system property.
   *
   * Default: false.
   */
  static auto BootAnimationEnabled() -> bool;
};

}  // namespace android::drm_hwcomposer
