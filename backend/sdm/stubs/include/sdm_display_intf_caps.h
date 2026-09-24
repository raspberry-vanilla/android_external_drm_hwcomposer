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

#include <core/buffer_allocator.h>
#include <core/sdm_types.h>

namespace sdm {

class SDMDisplayCapsIntf {
 public:
  virtual ~SDMDisplayCapsIntf() = default;
  virtual DisplayError GetHdrCapabilities(uint64_t disp_id,
                                          uint32_t *out_num_types,
                                          int32_t *out_types,
                                          float *out_max_luminance,
                                          float *out_max_average_luminance,
                                          float *out_min_luminance) = 0;
  virtual DisplayError GetDisplayHwId(uint64_t disp_id,
                                      int32_t *disp_hw_id) = 0;
};

}  // namespace sdm
