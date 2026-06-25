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

#pragma once

#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/mapper/CursorLayerMapper.h"
#include "compositor/mapper/ForceClientCompositionLayerMapper.h"
#include "compositor/mapper/LayerCachingMapper.h"
#include "compositor/mapper/LayerMapper.h"
#include "compositor/mapper/LeftoverLayerMapper.h"
#include "compositor/mapper/UnderlayMapper.h"
#include "mapper/LayerMapper.h"

namespace android::drm_hwcomposer {

enum class CompositionType;
class ICompositorDisplay;
class HwcLayer;

// Implementation of CompositionPlanner built on top of upstream drm uAPI and
// series of LayerMappers.
class GenericLayerMapperCompositionPlanner : public CompositionPlanner {
 public:
  explicit GenericLayerMapperCompositionPlanner(
      LayerMapper::MappingValidator backend_validator = nullptr);
  ~GenericLayerMapperCompositionPlanner() override = default;

  ValidationResult ValidateDisplay(const ICompositorDisplay* display) override;

 private:
  static std::vector<LayerMapping> MapAllClientCompositionRequiredLayers(
      const ICompositorDisplay* display,
      const std::vector<LayerMapping>& layers);

  ValidatedComposition CreateFlattenedComposition(
      const std::vector<LayerMapping>& layers,
      FlattenReason flatten_reason) const;

  bool ShouldUseCursorPlane(const ICompositorDisplay* display,
                            const std::vector<LayerMapping>& layers) const;

  const CursorLayerMapper& GetCursorMapper(bool use_cursor_plane) const;

  // Maps cursor layer to kCursor composition type.
  CursorLayerMapper cursor_mapper_;
  // Maps cursor layer to kDevice composition type as a fallback.
  CursorLayerMapper device_cursor_mapper_;
  ForceClientCompositionLayerMapper force_client_composition_mapper_;
  LayerCachingMapper layer_caching_mapper_;
  LeftoverLayerMapper leftover_mapper_;
  UnderlayMapper underlay_mapper_;
  LayerMapper::MappingValidator backend_validator_;
};

}  // namespace android::drm_hwcomposer
