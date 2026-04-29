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
#include "compositor/mapper/LayerCachingMapper.h"

#include <vector>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "compositor/mapper/MapperUtils.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

std::vector<LayerMapping> LayerCachingMapper::AssignLayers(
    const std::vector<LayerMapping>& layers,
    const MappingValidator& /*validator*/) const {
  std::vector<LayerMapping> new_layers = layers;
  for (auto& [layer, composition_type] : new_layers) {
    // Only proceed for device composition eligible layers.
    const CompositionType sf_type = layer->GetSfType();
    if (sf_type != CompositionType::kDevice &&
        sf_type != CompositionType::kDeviceOccluded) {
      continue;
    }

    if (IsLayerCached(*layer)) {
      composition_type = CompositionType::kDeviceOccluded;
    }
  }

  return new_layers;
}
};  // namespace android::drm_hwcomposer
