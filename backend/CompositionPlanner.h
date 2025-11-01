/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {

struct DrmKmsPlan;
class HwcDisplay;
class HwcLayer;

// CompositionPlanner is responsible for determining the mapping between
// HwcLayer and drm planes. This includes deciding which HwcLayers should be
// client composited, and which ones can be composited by the device.
class CompositionPlanner {
 public:
  // Mapping of the CompositionType that the Backend assigned to each
  // HwcLayer.
  using CompositionTypeMap = std::map<const HwcLayer*, CompositionType>;
  // Enum of possible reasons that the backend may choose to flatten the
  // composition.
  enum class FlattenReason {
    kUnspecified = 0,
    kNone,
    kStaticScene,
    kValidateFailed,
    kCtmWithOffset,
  };
  struct ValidatedComposition {
    // The resulting composition type for each layer.
    CompositionTypeMap composition_types{};
    // The DrmKms resources required for the composition. The lifetime of
    // the DrmKmsPlan ensures that corresponding drm resources are reserved
    // for use by this display. As such, the caller must ensure that the
    // DrmKmsPlan is not destructed before the composition is committed.
    std::shared_ptr<DrmKmsPlan> composition_plan = nullptr;
    // Reason the composition was flattened, or |kNone| if it wasn't.
    FlattenReason flatten_reason = FlattenReason::kNone;
    // Whether the cursor plane was successfully validated, or |nullopt| if it
    // wasn't attempted.
    std::optional<bool> cursor_plane_validated = std::nullopt;
  };

  virtual ~CompositionPlanner() = default;
  virtual ValidatedComposition ValidateDisplay(
      const HwcDisplay* display) const = 0;

  // Returns a ValidatedComposition that assigns all HwcLayers to client
  // composition.
  static ValidatedComposition GetFlattenedComposition(
      const std::vector<const HwcLayer*>& layers, FlattenReason flatten_reason);
};

}  // namespace android::drm_hwcomposer
