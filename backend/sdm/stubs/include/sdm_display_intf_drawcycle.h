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

#include <core/display_interface.h>
#include <core/layer_stack.h>
#include <core/sdm_types.h>
#include <utils/fence.h>

namespace sdm {

class SDMDisplayDrawCycleIntf {
 public:
  virtual ~SDMDisplayDrawCycleIntf() = default;
  virtual DisplayError SetActiveConfigIndex(uint64_t disp_id,
                                            uint32_t index) = 0;
  virtual DisplayError PresentDisplay(
      uint64_t disp_id, std::shared_ptr<Fence> *out_retire_fence) = 0;
  virtual DisplayError SetClientTarget(uint64_t disp_id,
                                       const SnapHandle *target,
                                       std::shared_ptr<Fence> acquire_fence,
                                       int32_t dataspace, SDMRegion damage,
                                       uint32_t version) = 0;
  virtual DisplayError WaitForDrawCycleToComplete(uint64_t disp_id) = 0;
  virtual DisplayError LayerStackUpdated(uint64_t disp_id) = 0;
  virtual DisplayError CommitOrPrepare(uint64_t disp_id, bool validate_only,
                                       std::shared_ptr<Fence> *out_retire_fence,
                                       uint32_t *out_num_types,
                                       uint32_t *out_num_requests,
                                       bool *out_needs_commit) = 0;
  virtual DisplayError AcceptDisplayChanges(uint64_t disp_id) = 0;
  virtual DisplayError GetChangedCompositionTypes(uint64_t disp_id,
                                                  uint32_t *out_num_elements,
                                                  int64_t *out_layers,
                                                  int32_t *out_types) = 0;
  virtual DisplayError GetDisplayRequests(uint64_t disp_id,
                                          int32_t *out_display_requests,
                                          uint32_t *out_num_elements,
                                          LayerId *out_layers,
                                          int32_t *out_layer_requests) = 0;
};

}  // namespace sdm
