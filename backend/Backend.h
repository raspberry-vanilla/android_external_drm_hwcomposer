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
#include <tuple>
#include <vector>

#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {

struct DrmKmsPlan;
class HwcDisplay;
class HwcLayer;

class Backend {
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

  virtual ~Backend() = default;
  virtual ValidatedComposition ValidateDisplay(const HwcDisplay* display) const;
  virtual std::tuple<size_t, size_t> GetClientLayers(
      const HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
      bool use_cursor_plane) const;
  virtual bool IsClientLayer(const HwcDisplay* display,
                             const HwcLayer* layer) const;

 protected:
  static ValidatedComposition GetFlattenedComposition(
      const std::vector<const HwcLayer*>& layers, FlattenReason flatten_reason);
  static bool HardwareSupportsLayerType(CompositionType comp_type);
  static uint32_t CalcPixOps(const std::vector<const HwcLayer*>& layers,
                             size_t first_z, size_t size);
  static CompositionTypeMap GetCompositionTypes(
      const std::vector<const HwcLayer*>& layers, size_t client_first_z,
      size_t client_size, bool use_cursor_plane);
  static std::tuple<size_t, size_t> GetExtraClientRange(
      const HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
      size_t client_start, size_t client_size, bool use_cursor_plane);
};

}  // namespace android::drm_hwcomposer
