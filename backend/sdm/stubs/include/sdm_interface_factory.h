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

#include <memory>

#include "sdm_display_intf_caps.h"
#include "sdm_display_intf_drawcycle.h"
#include "sdm_display_intf_layer_builder.h"
#include "sdm_display_intf_lifecycle.h"
#include "sdm_display_intf_settings.h"
#include "sdm_display_intf_sideband.h"

namespace sdm {

class SDMInterfaceFactory {
 public:
  virtual ~SDMInterfaceFactory() = default;
  virtual std::shared_ptr<SDMDisplayCapsIntf> CreateCapsIntf() = 0;
  virtual std::shared_ptr<SDMDisplayDrawCycleIntf> CreateDrawCycleIntf() = 0;
  virtual std::shared_ptr<SDMDisplayLayerBuilderIntf>
  CreateLayerBuilderIntf() = 0;
  virtual std::shared_ptr<SDMDisplaySettingsIntf> CreateSettingsIntf() = 0;
  virtual std::shared_ptr<SDMDisplayLifeCycleIntf> CreateLifeCycleIntf() = 0;
  virtual std::shared_ptr<SDMDisplaySideBandIntf> CreateSideBandIntf() = 0;
};

SDMInterfaceFactory *GetSDMInterfaceFactory();

}  // namespace sdm
