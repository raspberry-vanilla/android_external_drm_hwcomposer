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

#include <aidl/android/frameworks/stats/IStats.h>

namespace android::vendor::google::desktop::stats::DesktopAtoms {

enum AtomId {
  HWC_COMPOSITION_STATS,
  FLATTENING_STATE_CHANGED,
  COUNT_ACTIVE_DISPLAYS,
  DISPLAY_CONFIGURATION_RESULT,
  DISPLAY_HOTPLUG_CONNECT_MODE_DETECTED,
  DISPLAY_REFRESH_RATES_CHANGED,
};

struct HwcCompositionStats {
  enum ValidationResult {
    VALIDATION_RESULT_UNSPECIFIED,
    VALIDATION_RESULT_SUCCESS,
    VALIDATION_RESULT_FAILURE,
    VALIDATION_RESULT_SKIP,
  };

  enum FlattenReason {
    FLATTEN_REASON_UNSPECIFIED,
    FLATTEN_REASON_NONE,
    FLATTEN_REASON_STATIC_SCENE,
    FLATTEN_REASON_VALIDATE_FAILED,
    FLATTEN_REASON_CTM_WITH_OFFSET,
    FLATTEN_REASON_NO_PER_PLANE_COLORSPACE_SUPPORT,
  };
};

struct FlatteningStateChanged {
  enum FlatteningState {
    FLATTENING_STATE_UNSPECIFIED,
    FLATTENING_STATE_DISABLED,
    FLATTENING_STATE_ACTIVE,
    FLATTENING_STATE_FLATTENED,
  };
};

struct DisplayConfigurationResult {
  enum DisplayType {
    DISPLAY_TYPE_UNSPECIFIED,
    DISPLAY_TYPE_INTERNAL,
    DISPLAY_TYPE_EXTERNAL,
  };
};

struct DisplayHotplugConnectModeDetected {
  enum DisplayType {
    DISPLAY_TYPE_UNSPECIFIED,
    DISPLAY_TYPE_INTERNAL,
    DISPLAY_TYPE_EXTERNAL,
  };

  enum ConnectionType {
    CONNECTION_TYPE_UNSPECIFIED,
    CONNECTION_TYPE_VGA,
    CONNECTION_TYPE_DVII,
    CONNECTION_TYPE_DVID,
    CONNECTION_TYPE_DVIA,
    CONNECTION_TYPE_COMPOSITE,
    CONNECTION_TYPE_SVIDEO,
    CONNECTION_TYPE_LVDS,
    CONNECTION_TYPE_COMPONENT,
    CONNECTION_TYPE_9_PIN_DIN,
    CONNECTION_TYPE_DP_SST,
    CONNECTION_TYPE_DP_MST,
    CONNECTION_TYPE_HDMIA,
    CONNECTION_TYPE_HDMIB,
    CONNECTION_TYPE_TV,
    CONNECTION_TYPE_EDP,
    CONNECTION_TYPE_VIRTUAL,
    CONNECTION_TYPE_DSI,
    CONNECTION_TYPE_DPI,
    CONNECTION_TYPE_WRITEBACK,
    CONNECTION_TYPE_SPI,
    CONNECTION_TYPE_USB,
  };

  enum HdrType {
    HDR_TYPE_UNSPECIFIED,
    HDR_TYPE_DOLBY_VISION,
    HDR_TYPE_HDR10,
    HDR_TYPE_HLG,
    HDR_TYPE_HDR10_PLUS,
    HDR_TYPE_DOLBY_VISION_4K30,
    HDR_TYPE_HLG_PLUS,
  };
};

template <typename... Args>
inline ::aidl::android::frameworks::stats::VendorAtom createVendorAtom(
    int /*atom_id*/, const char* /*reverse_domain_name*/, Args&&... /*args*/) {
  return {};
}

}  // namespace android::vendor::google::desktop::stats::DesktopAtoms
