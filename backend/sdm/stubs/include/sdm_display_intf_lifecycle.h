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

#include <core/display_interface.h>
#include <core/socket_handler.h>
#include <utils/fence.h>

#include "debug_callback_intf.h"
#include "sdm_compositor_cb_intf.h"
#include "sdm_compositor_sideband_cb_intf.h"
#include "sdm_display_intf_layer_builder.h"

namespace sdm {

class SDMDisplayLifeCycleIntf {
 public:
  virtual ~SDMDisplayLifeCycleIntf() = default;
  virtual DisplayError Init(BufferAllocator *buffer_allocator,
                            SocketHandler *socket_handler,
                            DebugCallbackIntf *debug_callback) = 0;
  virtual void RegisterCompositorCallback(SDMCompositorCbIntf *cb,
                                          bool enable) = 0;
  virtual void RegisterSideBandCallback(SDMSideBandCompositorCbIntf *cb,
                                        bool enable) = 0;
  virtual DisplayError SetPowerMode(uint64_t disp_id, int32_t power_mode) = 0;
};

}  // namespace sdm
