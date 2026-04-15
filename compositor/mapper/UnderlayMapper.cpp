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
#include "compositor/mapper/UnderlayMapper.h"

#include <drm/drm_fourcc.h>

#include <vector>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 \
  fourcc_code('P', '0', '1', '0')
#endif

bool IsVideoBufferFormat(const HwcLayer* layer) {
  const auto& buffer_info = layer->GetLayerData().bi;
  return buffer_info.has_value() && (buffer_info->format == DRM_FORMAT_NV12 ||
                                     buffer_info->format == DRM_FORMAT_P010);
}
}  // namespace

std::vector<LayerMapping> UnderlayMapper::AssignLayers(
    const std::vector<LayerMapping>& layers,
    const MappingValidator& validator) const {
  std::vector<LayerMapping> new_mapping = layers;
  auto& [lowest_zpos_layer, composition_type] = new_mapping.front();

  // Only proceed for device composition eligible layers.
  const CompositionType sf_type = lowest_zpos_layer->GetSfType();
  if (sf_type != CompositionType::kDevice) {
    return new_mapping;
  }

  // If the layer was already marked as client, there is a reason why it can't
  // be device composited that should be resepcted.
  if (composition_type == CompositionType::kClient) {
    return new_mapping;
  }

  // TODO: account for platform-specific costs
  if (IsVideoBufferFormat(lowest_zpos_layer)) {
    composition_type = CompositionType::kDevice;
  }

  if (!validator(new_mapping)) {
    return layers;
  }

  return new_mapping;
}
};  // namespace android::drm_hwcomposer
