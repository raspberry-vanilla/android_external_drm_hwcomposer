/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "CompositionPlanner.h"

namespace android::drm_hwcomposer {

struct DrmKmsPlan;
class HwcDisplay;
class HwcLayer;

// Implementation of CompositionPlanner built on top of upstream drm uAPI.
class GenericCompositionPlanner : public CompositionPlanner {
 public:
  ~GenericCompositionPlanner() override = default;
  ValidatedComposition ValidateDisplay(
      const HwcDisplay* display) const override;

 private:
  static std::tuple<size_t, size_t> GetClientLayers(
      const HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
      bool use_cursor_plane);
  static bool IsClientLayer(const HwcDisplay* display, const HwcLayer* layer);

  static CompositionTypeMap GetCompositionTypes(
      const std::vector<const HwcLayer*>& layers, size_t client_first_z,
      size_t client_size, bool use_cursor_plane);
  static bool HardwareSupportsLayerType(CompositionType comp_type);
  static uint32_t CalcPixOps(const std::vector<const HwcLayer*>& layers,
                             size_t first_z, size_t size);
  static std::tuple<size_t, size_t> GetExtraClientRange(
      const HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
      size_t client_start, size_t client_size, bool use_cursor_plane);
};

}  // namespace android::drm_hwcomposer
