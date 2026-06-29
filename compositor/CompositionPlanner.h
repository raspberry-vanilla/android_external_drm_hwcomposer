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

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace android::drm_hwcomposer {

enum class CompositionType;
class HwcLayer;
class ICompositorDisplay;
struct DstRectInfo;
struct LayerToPlaneJoiningPlan;
struct SrcRectInfo;

struct LayerMapping;

// CompositionPlanner is responsible for determining the mapping between
// HwcLayer and drm planes. This includes deciding which HwcLayers should be
// client composited, and which ones can be composited by the device.
class CompositionPlanner {
 public:
  // Mapping of the CompositionType that the Backend assigned to each
  // HwcLayer.
  using CompositionTypeMap = std::map<const HwcLayer*, CompositionType>;
  // Enum of possible reasons that the planner may choose to flatten the
  // composition.
  enum class FlattenReason {
    kNone,
    kStaticScene,
    kValidateFailed,
    kCtmWithOffset,
    kNoPerPlaneColorspaceSupport,
  };
  struct ValidatedComposition {
    // The resulting composition type for each layer.
    CompositionTypeMap composition_types{};

    // Request the client to draw transparent pixels where these layers would
    // be.
    std::vector<const HwcLayer*> punch_out_layers{};

    // The DrmKms resources required for the composition. The lifetime of
    // the LayerToPlaneJoiningPlan ensures that corresponding drm resources are
    // reserved for use by this display. As such, the caller must ensure that
    // the LayerToPlaneJoiningPlan is not destructed before the composition is
    // committed.
    std::shared_ptr<LayerToPlaneJoiningPlan> composition_plan = nullptr;
    // Reason the composition was flattened, or |kNone| if it wasn't.
    FlattenReason flatten_reason = FlattenReason::kNone;
    // Whether the cursor plane was successfully validated, or |nullopt| if it
    // wasn't attempted.
    std::optional<bool> cursor_plane_validated = std::nullopt;
    // The error code in the event of validation failure. Indicates internal
    // failure iff |flatten_reason| is |kValidateFailed| and the error code is
    // 0.
    int error_code = 0;

    bool operator==(const ValidatedComposition& other) const {
      return composition_types == other.composition_types &&
             punch_out_layers == other.punch_out_layers &&
             composition_plan == other.composition_plan &&
             flatten_reason == other.flatten_reason &&
             cursor_plane_validated == other.cursor_plane_validated &&
             error_code == other.error_code;
    }
  };

  virtual ~CompositionPlanner() = default;

  struct ValidationResult {
    ValidatedComposition composition{};
    bool short_circuited = false;
  };
  // ValidateDisplay will be called at most once per frame update. It will not
  // be called again until the AtomicCommitArgs created from these
  // ValidatedComposition have been Executed through DrmAtomicCommitSink.
  virtual ValidationResult ValidateDisplay(
      const ICompositorDisplay* display) = 0;

  // Returns a ValidatedComposition that assigns all HwcLayers to client
  // composition.
  static ValidatedComposition GetFlattenedComposition(
      const std::vector<const HwcLayer*>& layers, FlattenReason flatten_reason);

  // Returns whether any two layers in |layers| have different colorspaces.
  template <typename Container>
  static bool LayersUseDifferentColorspaces(const Container& layers) {
    if (layers.empty()) {
      return false;
    }

    auto get_colorspace = [](const auto& item) {
      if constexpr (std::is_pointer_v<std::decay_t<decltype(item)>>) {
        return item->GetLayerData().colorspace;
      } else {
        return item.layer->GetLayerData().colorspace;
      }
    };

    const auto colorspace = get_colorspace(layers.front());
    return std::any_of(layers.begin() + 1, layers.end(),
                       [&](const auto& item) {
                         return get_colorspace(item) != colorspace;
                       });
  }
};

}  // namespace android::drm_hwcomposer
