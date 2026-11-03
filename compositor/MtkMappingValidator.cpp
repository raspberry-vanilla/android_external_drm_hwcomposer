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

#include "compositor/MtkMappingValidator.h"

#include <algorithm>
#include <vector>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"

namespace android::drm_hwcomposer {

bool MtkValidateMapping(const std::vector<LayerMapping>& test_layers) {
  return std::all_of(test_layers.begin(), test_layers.end(),
                     [](const auto& mapping) {
                       if (mapping.composition_type ==
                               CompositionType::kClient ||
                           mapping.composition_type ==
                               CompositionType::kInvalid ||
                           mapping.composition_type ==
                               CompositionType::kDeviceOccluded) {
                         return true;
                       }
                       const auto& pi = mapping.layer->GetLayerData().pi;
                       return !pi.RequireScalingOrPhasing();
                     });
}

}  // namespace android::drm_hwcomposer