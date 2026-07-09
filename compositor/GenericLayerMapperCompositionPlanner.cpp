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
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/FlatteningController.h"
#include "compositor/LayerData.h"
#include "compositor/ShortCircuitor.h"
#include "compositor/mapper/LayerMapper.h"
#include "compositor/mapper/MapperUtils.h"
#include "drm/CommitStatus.h"
#include "hwc/HwcLayer.h"

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

// Tests |proposed_layers| and updates |layers_to_update| and
// |composition_to_update| if the proposed layer composition is valid.
CommitStatus TestLayerMappings(
    std::vector<LayerMapping>&& proposed_layers,
    const ICompositorDisplay* display,
    std::vector<LayerMapping>& layers_to_update,
    std::optional<CompositionPlanner::ValidatedComposition>&
        composition_to_update) {
  CompositionPlanner::ValidatedComposition
      new_composition = CreateValidatedComposition(proposed_layers);

  const auto result = display->TestComposition(new_composition);
  if (result.success) {
    layers_to_update = std::move(proposed_layers);
    composition_to_update = std::move(new_composition);
  }

  return result;
}

// Updates |composition_to_update| if the proposed composition is valid.
CommitStatus TestLayerMappings(
    const std::vector<LayerMapping>& layers, const ICompositorDisplay* display,
    std::optional<CompositionPlanner::ValidatedComposition>&
        composition_to_update) {
  CompositionPlanner::ValidatedComposition
      new_composition = CreateValidatedComposition(layers);

  const auto result = display->TestComposition(new_composition);
  if (result.success) {
    composition_to_update = std::move(new_composition);
  }

  return result;
}

}  // namespace

GenericLayerMapperCompositionPlanner::GenericLayerMapperCompositionPlanner(
    LayerMapper::MappingValidator backend_validator)
    : cursor_mapper_(CompositionType::kCursor),
      device_cursor_mapper_(CompositionType::kDevice),
      backend_validator_(std::move(backend_validator)) {
}

CompositionPlanner::ValidationResult
GenericLayerMapperCompositionPlanner::ValidateDisplay(
    const ICompositorDisplay* display) {
  // An element with higher stack order is always in front of an element with a
  // lower stack order.
  const auto hwclayers = display->GetOrderLayersByZPos();
  std::vector<LayerMapping> layers = CreateZOrderedLayerMapping(hwclayers);

  // Early check and exit for flattened scenes.
  const FlatteningController* flatcon = display->GetFlatCon();
  if (flatcon != nullptr && flatcon->ShouldFlatten()) {
    return {.composition = CreateFlattenedComposition(layers, FlattenReason::
                                                                  kStaticScene),
            .short_circuited = false};
  }

  if (display->CtmByGpu()) {
    return {.composition = CreateFlattenedComposition(layers,
                                                      FlattenReason::
                                                          kCtmWithOffset),
            .short_circuited = false};
  }

  if (CompositionPlanner::LayersUseDifferentColorspaces(layers) && !display->UseColorPipeline()) {
    return {.composition = CreateFlattenedComposition(
                layers, FlattenReason::kNoPerPlaneColorspaceSupport),
            .short_circuited = false};
  }

  // Check if short-circuit is applicable in this validation request. If so,
  // skip the validation and use the previously presented composition.
  {
    auto last_presented = ShortCircuitor::
        Get(ShortCircuitor::Config::FromProperties(),
            display->GetLastPresentedComposition(),
            ValidationRequestContext(*display, hwclayers));
    if (last_presented) {
      return {.composition = std::move(*last_presented),
              .short_circuited = true};
    }
  }

  layers = MapAllClientCompositionRequiredLayers(display, layers);

  const LayerMapper::MappingValidator validator =
      [display, this](const std::vector<LayerMapping>& layers) {
        return CountRemainingPlanes(display, layers) >= 0 &&
               (backend_validator_ ? backend_validator_(layers) : true);
      };

  const bool use_cursor_plane = ShouldUseCursorPlane(display, layers);
  layers = GetCursorMapper(use_cursor_plane).AssignLayers(layers, validator);

  // Mapping dealing with layer caching does not need any testing as they do
  // not consume actual hardware resources.
  layers = layer_caching_mapper_.AssignLayers(layers, validator);

  std::optional<ValidatedComposition> validated_composition = std::nullopt;
  CommitStatus commit_status;

  {
    auto new_layers = underlay_mapper_.AssignLayers(layers, validator);
    commit_status = TestLayerMappings(std::move(new_layers), display, layers,
                                      validated_composition);
  }

  if (auto new_layers = leftover_mapper_.AssignLayers(layers, validator);
      new_layers != layers) {
    commit_status = TestLayerMappings(std::move(new_layers), display, layers,
                                      validated_composition);
  }

  // If UnderlayMapper and LeftoverMapper didn't produce a valid composition,
  // convert all unmapped layers into client composited layers and try.
  if (!validated_composition) {
    commit_status = TestLayerMappings(layers, display, validated_composition);
  }

  // Cursor fallback: convert all non-cursor layers to client composition and
  // reattempt.
  // The cursor layer is preserved as _either_ cursor _or_ device composited.
  if (!validated_composition && GetCursorLayer(layers) != nullptr) {
    if (IsCursorPlaneUsed(layers)) {
      layers = force_client_composition_mapper_.AssignLayers(layers, validator);
      layers.back().composition_type = CompositionType::kInvalid;
      layers = GetCursorMapper(use_cursor_plane)
                   .AssignLayers(layers, validator);

      ValidatedComposition new_composition = ValidatedComposition{
          .composition_types = ToCompositionTypes(layers)};
      commit_status = display->TestComposition(new_composition);
      if (commit_status.success) {
        validated_composition = std::move(new_composition);
      }
    }
  }

  const bool success_before_flattening = validated_composition.has_value();

  // Final fallback: convert all layers to client composition.
  if (!success_before_flattening) {
    constexpr auto kFlattenReason = FlattenReason::kValidateFailed;
    validated_composition = CreateFlattenedComposition(layers, kFlattenReason);
    validated_composition->error_code = commit_status.error_code;
  }

  if (use_cursor_plane) {
    validated_composition->cursor_plane_validated = success_before_flattening;
  }
  validated_composition->composition_plan.reset();
  return {.composition = std::move(*validated_composition),
          .short_circuited = false};
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
  if (DisplayCanUseCursorPlane(display, GetCursorLayer(layers))) {
    // Create and test a composition using only cursor plane and all other
    // layers client-composited to infer whether the cursor plane can be used.
    auto test_mappings = force_client_composition_mapper_
                             .AssignLayers(layers, NoOpValidator);
    test_mappings.back().composition_type = CompositionType::kCursor;

    ValidatedComposition cursor_composition{
        .composition_types = ToCompositionTypes(test_mappings)};
    return display->TestComposition(cursor_composition).success;
  }

  return false;
}

const CursorLayerMapper& GenericLayerMapperCompositionPlanner::GetCursorMapper(
    bool use_cursor_plane) const {
  return use_cursor_plane ? cursor_mapper_ : device_cursor_mapper_;
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
