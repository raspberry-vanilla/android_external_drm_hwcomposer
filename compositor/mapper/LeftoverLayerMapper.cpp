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
#include "compositor/mapper/LeftoverLayerMapper.h"

#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {
std::vector<LayerMapping> LeftoverLayerMapper::AssignLayers(
    const std::vector<LayerMapping>& layers,
    const MappingValidator& validator) const {
  std::optional<size_t> invalid_layer_index = std::nullopt;
  for (size_t i = 0; i < layers.size(); i++) {
    switch (layers[i].composition_type) {
      case CompositionType::kInvalid: {
        if (invalid_layer_index.has_value()) {
          // If there are more than one remaining/invalid layers, then the
          // leftover conversion results in more plane usage.
          return layers;
        }

        invalid_layer_index = i;
        continue;
      }
      case CompositionType::kDevice:
      case CompositionType::kDeviceOccluded:
      case CompositionType::kCursor:
        continue;
      case CompositionType::kClient:
      // Solid color is not supported right now, assume client composition.
      case CompositionType::kSolidColor:
        // Non-device-composited layer detected, no leftover layer cleanup
        // necessary as client composition is going to happen anyways.
        return layers;
    }
  }

  // Only attempt to convert a layer to device composition if the number of
  // planes used is going to be unchanged vs if client composition occured.
  if (!invalid_layer_index.has_value()) {
    return layers;
  }

  std::vector<LayerMapping> new_layers = layers;
  new_layers[*invalid_layer_index].composition_type = CompositionType::kDevice;

  return validator(new_layers) ? new_layers : layers;
}

}  // namespace android::drm_hwcomposer
