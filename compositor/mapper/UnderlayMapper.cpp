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

#include <chrono>
#include <vector>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "compositor/mapper/MapperUtils.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 fourcc_code('P', '0', '1', '0')
#endif

bool IsVideoBufferFormat(const HwcLayer* layer) {
  const auto& buffer_info = layer->GetLayerData().bi;
  return buffer_info.has_value() && (buffer_info->format == DRM_FORMAT_NV12 ||
                                     buffer_info->format == DRM_FORMAT_P010);
}

// TODO: front-buffered layers should always be considered to be MAX_FPS
bool IsUnderlayHotspot(const std::vector<LayerMapping>& layers) {
  bool has_layer_caching = false;
  for (const auto& [layer, composition_type] : layers) {
    if (composition_type == CompositionType::kDevice ||
        composition_type == CompositionType::kCursor) {
      continue;
    }

    if (IsLayerCached(*layer)) {
      has_layer_caching = true;
      break;
    }
  }

  if (!has_layer_caching) {
    return false;
  }

  // Hole-punched underlay candidate from SurfaceFlinger, if present, will
  // always at the bottom of the layer stack. HWC is unable to directly discern
  // if layer-caching hole-punching is in effect, so we must take a guess. If
  // layer caching is enabled and the underlay candidate is very active while
  // all the other layers are inactive, then the underlay candidate should be
  // promoted.
  const auto& [candidate_layer, candidate_composition_type] = layers.front();
  // TODO: inject clock for testing
  const auto now = std::chrono::steady_clock::now();
  const float underlay_fps = candidate_layer->GetLayerData()
                                 .frame_time_history.CalculateFps(now);

  // TODO: Make |kActiveFpsThreshold| tuneable.
  constexpr float kActiveFpsThreshold = 12.0F;
  if (underlay_fps < kActiveFpsThreshold) {
    return false;
  }

  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (const auto& [layer, _] : layers) {
    // Skip the underlay itself
    if (layer == candidate_layer) {
      continue;
    }

    // Cursor movement is exempt from inactivity calculation
    if (layer->GetSfType() == CompositionType::kCursor) {
      continue;
    }

    if (IsLayerCached(*layer)) {
      continue;
    }

    // All layers other than the underlay candidate must be inactive.
    // |kInactiveFpsThreshold| is based off of the inactive layer threshold used
    // by SurfaceFlinger for layer caching.
    constexpr float kInactiveFpsThreshold = 1.0F;
    const float layer_fps = layer->GetLayerData()
                                .frame_time_history.CalculateFps(now);
    if (layer_fps > kInactiveFpsThreshold) {
      return false;
    }
  }

  return true;
}
}  // namespace

std::vector<LayerMapping> UnderlayMapper::AssignLayers(
    const std::vector<LayerMapping>& layers,
    const MappingValidator& validator) const {
  std::vector<LayerMapping> new_mapping = layers;

  // Hole-punched underlay candidate from SurfaceFlinger, if present, will
  // always be at the bottom of the layer stack.
  auto& [candidate_layer, candidate_composition_type] = new_mapping.front();

  // Only proceed for device composition eligible layers.
  const CompositionType sf_type = candidate_layer->GetSfType();
  if (sf_type != CompositionType::kDevice) {
    return new_mapping;
  }

  // If the layer was already marked as client, there is a reason why it can't
  // be device composited that should be resepcted.
  if (candidate_composition_type == CompositionType::kClient) {
    return new_mapping;
  }

  // TODO: account for platform-specific costs
  if (IsVideoBufferFormat(candidate_layer) || IsUnderlayHotspot(new_mapping)) {
    candidate_composition_type = CompositionType::kDevice;
  }

  if (!validator(new_mapping)) {
    return layers;
  }

  return new_mapping;
}
};  // namespace android::drm_hwcomposer
