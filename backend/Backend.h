/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <map>
#include <vector>

#include "compositor/LayerData.h"

namespace android {

class HwcDisplay;
class HwcLayer;

class Backend {
 public:
  // Mapping of the CompositionType that the Backend assigned to each
  // HwcLayer.
  using CompositionTypeMap = std::map<const HwcLayer*, CompositionType>;

  virtual ~Backend() = default;
  virtual CompositionTypeMap ValidateDisplay(HwcDisplay* display);
  virtual std::tuple<int, size_t> GetClientLayers(
      HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
      bool use_cursor_plane);
  virtual bool IsClientLayer(HwcDisplay* display, const HwcLayer* layer);

 protected:
  static bool HardwareSupportsLayerType(CompositionType comp_type);
  static uint32_t CalcPixOps(const std::vector<const HwcLayer*>& layers,
                             size_t first_z, size_t size,
                             std::pair<uint32_t, uint32_t> display_size);
  static CompositionTypeMap GetCompositionTypes(
      const std::vector<const HwcLayer*>& layers, size_t client_first_z,
      size_t client_size, bool use_cursor_plane);
  static std::tuple<int, int> GetExtraClientRange(
      HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
      int client_start, size_t client_size, bool use_cursor_plane);
};
}  // namespace android
