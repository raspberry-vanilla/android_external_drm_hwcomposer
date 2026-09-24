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
#include <vector>

#include <core/display_interface.h>
#include <core/sdm_types.h>

namespace sdm {

class SDMDisplaySettingsIntf {
 public:
  virtual ~SDMDisplaySettingsIntf() = default;
  virtual DisplayError SetIdleTimeout(uint32_t timeout_ms) = 0;
  virtual DisplayError GetColorModes(uint64_t disp_id, uint32_t *out_num_modes,
                                     int32_t *out_modes) = 0;
  virtual DisplayError GetAllDisplayAttributes(
      uint64_t disp_id,
      std::map<uint32_t, DisplayConfigVariableInfo> *out_configs) = 0;
  virtual DisplayError GetActiveConfig(uint64_t disp_id,
                                       uint32_t *out_config_id) = 0;
  virtual DisplayError SetActiveConfig(uint64_t disp_id,
                                       uint32_t config_id) = 0;
  virtual DisplayError SetColorModeWithRenderIntent(uint64_t disp_id,
                                                    int32_t color_mode,
                                                    int32_t render_intent) = 0;
  virtual DisplayError SetColorTransform(uint64_t disp_id,
                                         const std::vector<float> &matrix) = 0;
  virtual int GetDisplayConfigGroup(
      uint64_t disp_id, const DisplayConfigVariableInfo &config) = 0;
};

}  // namespace sdm
