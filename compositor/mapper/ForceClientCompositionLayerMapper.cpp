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
#include "compositor/mapper/ForceClientCompositionLayerMapper.h"

#include "compositor/LayerData.h"

namespace android::drm_hwcomposer {
std::vector<LayerMapping> ForceClientCompositionLayerMapper::AssignLayers(
    const std::vector<LayerMapping>& layers,
    const MappingValidator& /*validator*/) const {
  std::vector<LayerMapping> new_layers = layers;
  for (auto& layer : new_layers) {
    layer.composition_type = CompositionType::kClient;
  }

  return new_layers;
}
};  // namespace android::drm_hwcomposer
