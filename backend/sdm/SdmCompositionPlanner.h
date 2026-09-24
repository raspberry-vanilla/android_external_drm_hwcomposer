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

#include <cstdint>
#include <map>

#include "compositor/CompositionPlanner.h"

namespace sdm {
class SDMDisplayLayerBuilderIntf;
class SDMDisplayDrawCycleIntf;
}  // namespace sdm

namespace android::drm_hwcomposer {

enum class CompositionType;
struct LayerData;

// CompositionPlanner implementation that interfaces with SDM.
// The HwcLayer and LayerData properties are plumbed down to SDM using
// the SDMDisplayLayerBuilderIntf, and composition strategy is deferred
// to SDMDisplayDrawCycleIntf.
class SdmCompositionPlanner : public CompositionPlanner {
 public:
  SdmCompositionPlanner(uint64_t sdm_display_id,
                        sdm::SDMDisplayLayerBuilderIntf* layer_intf,
                        sdm::SDMDisplayDrawCycleIntf* draw_cycle_intf);
  ~SdmCompositionPlanner() override;

  auto ValidateDisplay(const ICompositorDisplay* display)
      -> ValidationResult override;

 private:
  using SDMLayerId = int64_t;

  void UpdateLayerMapping(const ICompositorDisplay& display);
  void UpdateLayer(const HwcLayer* hwc_layer, CompositionType composition_type);

  void UpdateLayerBuffer(SDMLayerId sdm_layer_id, const LayerData& layer_data);
  void UpdateLayerBlendMode(SDMLayerId sdm_layer_id,
                            const LayerData& layer_data);
  void UpdateLayerDisplayFrame(SDMLayerId sdm_layer_id,
                               const LayerData& layer_data);
  void UpdateLayerSolidColor(SDMLayerId sdm_layer_id,
                             const LayerData& layer_data);
  void UpdateLayerAlpha(SDMLayerId sdm_layer_id, const LayerData& layer_data);
  void UpdateLayerSourceCrop(SDMLayerId sdm_layer_id,
                             const LayerData& layer_data);
  void UpdateLayerTransform(SDMLayerId sdm_layer_id,
                            const LayerData& layer_data);
  void UpdateZOrder(SDMLayerId sdm_layer_id, uint32_t z_order);
  void UpdateCompositionType(SDMLayerId sdm_layer_id,
                             CompositionType composition_type);
  void UpdateLayerDataspace(SDMLayerId sdm_layer_id,
                            const LayerData& layer_data);
  void UpdateLayerBrightness(SDMLayerId sdm_layer_id,
                             const LayerData& layer_data);

  const HwcLayer* GetHwcLayer(SDMLayerId sdm_layer_id) const;

  uint64_t sdm_display_id_;
  std::map<const HwcLayer*, SDMLayerId> layer_mappings_;

  sdm::SDMDisplayLayerBuilderIntf* layer_intf_;
  sdm::SDMDisplayDrawCycleIntf* draw_cycle_intf_;
};

}  // namespace android::drm_hwcomposer