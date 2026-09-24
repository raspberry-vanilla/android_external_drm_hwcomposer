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

#include <core/sdm_types.h>

namespace sdm {

class SDMCompositorCbIntf {
 public:
  virtual ~SDMCompositorCbIntf() = default;
  virtual void OnHotplug(uint64_t in_display, bool in_connected) = 0;
  virtual void OnRefresh(uint64_t in_display) = 0;
  virtual void OnVsync(uint64_t in_display, int64_t in_timestamp,
                       int32_t in_vsync_period_nanos) = 0;
  virtual void OnSeamlessPossible(uint64_t in_display) = 0;
  virtual void OnVsyncIdle(uint64_t in_display) = 0;
  virtual void OnVsyncPeriodTimingChanged(
      uint64_t in_display, const SDMVsyncPeriodChangeTimeline &timeline) = 0;
};

}  // namespace sdm
