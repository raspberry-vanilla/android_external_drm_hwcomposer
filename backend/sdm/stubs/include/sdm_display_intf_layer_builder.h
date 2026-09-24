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
#include <memory>

#include <SnapHandle.h>
#include <core/buffer_allocator.h>
#include <core/layer_stack.h>
#include <core/sdm_types.h>
#include <utils/fence.h>

namespace sdm {

class SDMDisplayLayerBuilderIntf {
 public:
  virtual ~SDMDisplayLayerBuilderIntf() = default;
  virtual DisplayError Init(BufferAllocator *buffer_allocator,
                            uint64_t disp_id) = 0;
  virtual DisplayError CreateLayer(uint64_t disp_id, int64_t *out_layer_id) = 0;
  virtual DisplayError DestroyLayer(uint64_t disp_id, int64_t layer_id) = 0;
  virtual DisplayError SetLayerBuffer(
      uint64_t disp_id, int64_t layer_id,
      vendor::qti::hardware::display::snapalloc::SnapHandle *buffer,
      std::shared_ptr<Fence> acquire_fence) = 0;
  virtual DisplayError SetLayerBlendMode(uint64_t disp_id, int64_t layer_id,
                                         LayerBlending blend_mode) = 0;
  virtual DisplayError SetLayerDisplayFrame(uint64_t disp_id, int64_t layer_id,
                                            SDMRect frame) = 0;
  virtual DisplayError SetLayerColor(uint64_t disp_id, int64_t layer_id,
                                     SDMColor color) = 0;
  virtual DisplayError SetLayerPlaneAlpha(uint64_t disp_id, int64_t layer_id,
                                          float alpha) = 0;
  virtual DisplayError SetLayerSourceCrop(uint64_t disp_id, int64_t layer_id,
                                          SDMRect crop) = 0;
  virtual DisplayError SetLayerTransform(uint64_t disp_id, int64_t layer_id,
                                         SDMTransform transform) = 0;
  virtual DisplayError SetLayerZOrder(uint64_t disp_id, int64_t layer_id,
                                      uint32_t z_order) = 0;
  virtual DisplayError SetLayerCompositionType(uint64_t disp_id,
                                               int64_t layer_id,
                                               int32_t type) = 0;
  virtual DisplayError SetLayerDataspace(uint64_t disp_id, int64_t layer_id,
                                         int32_t dataspace) = 0;
  virtual DisplayError SetLayerBrightness(uint64_t disp_id, int64_t layer_id,
                                          float brightness) = 0;
};

}  // namespace sdm
