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
#include "compositor/mapper/MapperUtils.h"

#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

bool IsLayerCached(const HwcLayer& layer) {
  const float alpha = layer.GetLayerData().pi.alpha;
  // The implicit contract between SF and HWC on layer caching is that cached
  // layers are set to have 0.0f alpha, except for one in the set representing
  // all layers.
  constexpr float kCachedLayerOpacity = 0.0F;
  return alpha == kCachedLayerOpacity;
}
};  // namespace android::drm_hwcomposer
