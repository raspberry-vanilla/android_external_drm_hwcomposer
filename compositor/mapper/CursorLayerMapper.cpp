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
#include "compositor/mapper/CursorLayerMapper.h"

#include <vector>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
std::vector<LayerMapping> CursorLayerMapper::AssignLayers(
    const std::vector<LayerMapping>& layers,
    const MappingValidator& validator) const {
  std::vector<LayerMapping> new_mapping = layers;
  LayerMapping& highest_zpos_layer = new_mapping.back();
  if (highest_zpos_layer.composition_type == CompositionType::kClient) {
    return layers;
  }

  if (highest_zpos_layer.layer->GetSfType() == CompositionType::kCursor) {
    highest_zpos_layer.composition_type = cursor_plane_type_;
  }

  if (!validator(new_mapping)) {
    return layers;
  }

  return new_mapping;
}
};  // namespace android::drm_hwcomposer
