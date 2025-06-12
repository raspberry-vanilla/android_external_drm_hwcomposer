/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <optional>
#include <string>

namespace android {

enum class CtmHandling {
  kDrmOrGpu,    /* Handled by DRM is possible, otherwise by GPU */
  kDrmOrIgnore, /* Handled by DRM is possible, otherwise displayed as is */
};

class Properties {
 public:
  static auto IsPresentFenceNotReliable() -> bool;
  static auto UseConfigGroups() -> bool;
  static auto InternalDisplayNames() -> std::string;
  static auto UseOverlayPlanes() -> bool;
  static auto ScaleWithGpu() -> bool;
  static auto EnableVirtualDisplay() -> bool;
  static auto GetCtmHandling() -> CtmHandling;
  static auto GetBackendOverride() -> std::string;
  static auto GetDevicePath() -> std::string;
};

}  // namespace android
