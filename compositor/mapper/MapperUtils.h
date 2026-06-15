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

#include "compositor/LayerData.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

// Returns true if |layer| is considered cached by SurfaceFlinger.
[[nodiscard]] bool IsLayerCached(const HwcLayer& layer);

[[nodiscard]] bool HardwareSupportsLayerType(CompositionType comp_type);

[[nodiscard]] bool MustBeClientComposited(const ICompositorDisplay* display,
                                          const HwcLayer* layer);

// Heuristically checks if the display can use the cursor plane,
// without issuing a test in kernel.
[[nodiscard]] bool DisplayCanUseCursorPlane(const ICompositorDisplay* display,
                                            const HwcLayer* cursor_layer);
}  // namespace android::drm_hwcomposer
