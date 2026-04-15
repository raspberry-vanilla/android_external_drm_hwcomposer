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

#include <functional>
#include <vector>

#include "compositor/LayerData.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

struct LayerMapping {
  const HwcLayer* layer = nullptr;
  CompositionType composition_type = CompositionType::kInvalid;
};

class LayerMapper {
 public:
  virtual ~LayerMapper() = default;

  // MappingValidator allows the caller of AssignLayers() to assess the proposed
  // layer mappings before LayerMapper returns so that the LayerMapper could
  // adjust its mapping if it is not going to be acceptable.
  // For example, a MappingValidator can return false if the proposed mapping
  // exceeds the number of overlays available on a device.
  using MappingValidator = std::function<bool(
      const std::vector<LayerMapping>&)>;

  virtual std::vector<LayerMapping> AssignLayers(
      const std::vector<LayerMapping>& layers,
      const MappingValidator& validator) const = 0;

  // TOOD: dumpsys interface
  // TODO: metrics interface
};
}  // namespace android::drm_hwcomposer
