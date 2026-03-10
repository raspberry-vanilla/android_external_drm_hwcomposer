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
#include "GenericLayerMapperCompositionPlanner.h"

#include <algorithm>
#include <vector>

#include "compositor/FlatteningController.h"
#include "compositor/LayerData.h"
#include "drm/DrmPlane.h"
#include "hwc/HwcLayer.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {

std::vector<LayerMapping> CreateZOrderedLayerMapping(
    const std::vector<const HwcLayer*>& layers) {
  std::vector<LayerMapping> mapping;
  mapping.reserve(layers.size());
  for (const auto* layer : layers) {
    mapping.push_back({layer, CompositionType::kInvalid});
  }

  // Sort from lowest to highest Z.
  // This is likely a no-op as the layers are passed in sorted.
  std::stable_sort(mapping.begin(), mapping.end(),
                   [](const LayerMapping& lhs, const LayerMapping& rhs) {
                     return lhs.layer->GetZOrder() < rhs.layer->GetZOrder();
                   });

  return mapping;
}

CompositionPlanner::CompositionTypeMap ToCompositionTypes(
    const std::vector<LayerMapping>& layers) {
  CompositionPlanner::CompositionTypeMap composition_types;
  for (const auto& [layer, composition_type] : layers) {
    composition_types[layer] = composition_type;
  }
  return composition_types;
}

// Requires z-ordered, non-empty |layers|.
const HwcLayer* GetCursorLayer(const std::vector<LayerMapping>& layers) {
  // If a cursor layer is present, it has the highest Z-order.
  const auto& cursor_candidate = layers.back();
  return cursor_candidate.layer->GetSfType() == CompositionType::kCursor
             ? cursor_candidate.layer
             : nullptr;
}

bool IsCursorPlaneUsed(const std::vector<LayerMapping>& layers) {
  // |layers| is sorted from lowest to highest Z. If a cursor is present, then
  // it must have the highest Z and therefore can only exist at the back.
  return layers.back().composition_type == CompositionType::kCursor;
}

// Convert all undetermined layers into client layers.
std::vector<LayerMapping> InvalidToClientLayers(
    const std::vector<LayerMapping>& layers) {
  std::vector<LayerMapping> new_mapping = layers;
  for (auto& [_, composition_type] : new_mapping) {
    if (composition_type == CompositionType::kInvalid) {
      composition_type = CompositionType::kClient;
    }
  }

  return new_mapping;
}

CompositionPlanner::ValidatedComposition CreateValidatedComposition(
    const std::vector<LayerMapping>& layers) {
  CompositionPlanner::ValidatedComposition validated_composition = {
      .composition_types = ToCompositionTypes(InvalidToClientLayers(layers))};
  return validated_composition;
}

// If >= 0, then the mapping described by |layers| is valid.
// If < 0, then |layers| is invalid as it uses more planes than available.
int CountRemainingPlanes(const ICompositorDisplay* display,
                         const std::vector<LayerMapping>& layers) {
  int num_available_planes = static_cast<int>(display->GetNumAvailablePlanes());

  bool has_client_layers = false;
  for (const auto& [_, composition_type] : layers) {
    // Specifically exclude kDeviceOccluded as they don't use up planes.
    if (composition_type == CompositionType::kDevice) {
      // TODO: account for platform-specific layer costing.
      num_available_planes--;
    } else if (composition_type == CompositionType::kClient ||
               // Invalid layers should be treated as client composited to be
               // conservative.
               composition_type == CompositionType::kInvalid) {
      has_client_layers = true;
    }
  }

  if (has_client_layers) {
    num_available_planes--;
  }

  return num_available_planes;
}

bool NoOpValidator(const std::vector<LayerMapping>& /*unused*/) {
  return true;
}
}  // namespace

GenericLayerMapperCompositionPlanner::GenericLayerMapperCompositionPlanner()
    : cursor_mapper_(CompositionType::kCursor),
      device_cursor_mapper_(CompositionType::kDevice) {
}

CompositionPlanner::ValidatedComposition
GenericLayerMapperCompositionPlanner::ValidateDisplay(
    const ICompositorDisplay* display) {
  // An element with higher stack order is always in front of an element with a
  // lower stack order.
  std::vector<LayerMapping> layers = CreateZOrderedLayerMapping(
      display->GetOrderLayersByZPos());

  // Early check and exit for flattened scenes.
  const FlatteningController* flatcon = display->GetFlatCon();
  if (flatcon != nullptr && flatcon->ShouldFlatten()) {
    return CreateFlattenedComposition(layers, FlattenReason::kStaticScene);
  }

  if (display->CtmByGpu()) {
    return CreateFlattenedComposition(layers, FlattenReason::kCtmWithOffset);
  }

  // If there's only one layer, no need to use the GPU to client composite.
  if (layers.size() == 1 &&
      !MustBeClientComposited(display, layers.front().layer)) {
    layers.front().composition_type = CompositionType::kDevice;
    return ValidatedComposition{
        .composition_types = ToCompositionTypes(layers)};
  }

  layers = MapAllClientCompositionRequiredLayers(display, layers);

  LayerMapper::MappingValidator validator =
      [display](const std::vector<LayerMapping>& layers) {
        return CountRemainingPlanes(display, layers) >= 0;
      };

  const bool use_cursor_plane = ShouldUseCursorPlane(display, layers);
  if (use_cursor_plane) {
    layers = cursor_mapper_.AssignLayers(layers, validator);
  } else {
    layers = device_cursor_mapper_.AssignLayers(layers, validator);
  }

  layers = layer_caching_mapper_.AssignLayers(layers, validator);

  {
    auto new_layers = underlay_mapper_.AssignLayers(layers, validator);

    ValidatedComposition test_composition = CreateValidatedComposition(
        new_layers);
    if (display->TestComposition(test_composition)) {
      layers = new_layers;
    }
  }

  // Convert all unmapped layers into client composited layers.
  ValidatedComposition validated_composition = CreateValidatedComposition(
      layers);
  bool success = display->TestComposition(validated_composition);
  validated_composition.composition_plan.reset();

  // Cursor fallback: convert all non-cursor layers to client composition and
  // reattempt.
  // The cursor layer is preserved as _either_ cursor _or_ device composited.
  if (!success && GetCursorLayer(layers) != nullptr) {
    if (IsCursorPlaneUsed(layers)) {
      layers = force_client_composition_mapper_.AssignLayers(layers, validator);
      layers.back().composition_type = CompositionType::kInvalid;
      if (use_cursor_plane) {
        layers = cursor_mapper_.AssignLayers(layers, validator);
      } else {
        layers = device_cursor_mapper_.AssignLayers(layers, validator);
      }

      validated_composition.composition_types = ToCompositionTypes(layers);
      success = display->TestComposition(validated_composition);
      validated_composition.composition_plan.reset();
    }
  }

  // Final fallback: convert all layers to client composition.
  if (!success) {
    validated_composition = CreateFlattenedComposition(layers,
                                                       FlattenReason::
                                                           kValidateFailed);
  }

  if (use_cursor_plane) {
    validated_composition.cursor_plane_validated = success;
  }

  return validated_composition;
}

bool GenericLayerMapperCompositionPlanner::MustBeClientComposited(
    const ICompositorDisplay* display, const HwcLayer* layer) {
  // As per Composition.aidl, if Composition is CLIENT, HWC is not allowed to
  // request a change.
  return !HardwareSupportsLayerType(layer->GetSfType()) ||
         !layer->IsLayerUsableAsDevice() || display->CtmByGpu() ||
         (layer->GetLayerData().pi.RequireScalingOrPhasing() &&
          display->ForcedScalingWithGpu());
}

bool GenericLayerMapperCompositionPlanner::HardwareSupportsLayerType(
    CompositionType comp_type) {
  return comp_type == CompositionType::kDevice ||
         comp_type == CompositionType::kCursor ||
         comp_type == CompositionType::kDeviceOccluded;
}

CompositionPlanner::ValidatedComposition
GenericLayerMapperCompositionPlanner::CreateFlattenedComposition(
    const std::vector<LayerMapping>& layers,
    FlattenReason flatten_reason) const {
  return ValidatedComposition{.composition_types = ToCompositionTypes(
                                  force_client_composition_mapper_
                                      .AssignLayers(layers, NoOpValidator)),
                              .composition_plan = nullptr,
                              .flatten_reason = flatten_reason};
}

bool GenericLayerMapperCompositionPlanner::ShouldUseCursorPlane(
    const ICompositorDisplay* display,
    const std::vector<LayerMapping>& layers) const {
  const auto* cursor_layer = GetCursorLayer(layers);
  const auto cursor_plane = display->GetCursorPlane();
  if (cursor_layer != nullptr && cursor_plane != nullptr &&
      !MustBeClientComposited(display, cursor_layer) &&
      cursor_plane->Get()->IsValidForLayer(&cursor_layer->GetLayerData())) {
    // Create and test a composition using only cursor plane and all other
    // layers client-composited to infer whether the cursor plane can be used.
    auto test_mappings = force_client_composition_mapper_
                             .AssignLayers(layers, NoOpValidator);
    test_mappings.back().composition_type = CompositionType::kCursor;

    ValidatedComposition cursor_composition{
        .composition_types = ToCompositionTypes(test_mappings)};
    return display->TestComposition(cursor_composition);
  }

  return false;
}

std::vector<LayerMapping>
GenericLayerMapperCompositionPlanner::MapAllClientCompositionRequiredLayers(
    const ICompositorDisplay* display,
    const std::vector<LayerMapping>& layers) {
  std::vector<LayerMapping> new_layers = layers;
  for (auto& [layer, composition_type] : new_layers) {
    if (MustBeClientComposited(display, layer)) {
      composition_type = CompositionType::kClient;
    }
  }

  return new_layers;
}

}  // namespace android::drm_hwcomposer
