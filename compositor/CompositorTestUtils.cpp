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

#include "compositor/CompositorTestUtils.h"

namespace android::drm_hwcomposer {

HwcLayer CompositorTestUtils::CreateLayer(ICompositorDisplay* display,
                                          IRect dest_ltrb, uint32_t z_order,
                                          CompositionType type, float alpha,
                                          uint32_t buffer_format) {
  HwcLayer layer(display);
  HwcLayer::LayerProperties props{
      .buffer = HwcLayer::
          Buffer{.bi = BufferInfo{.width = static_cast<uint32_t>(
                                      dest_ltrb.right - dest_ltrb.left),
                                  .height = static_cast<uint32_t>(
                                      dest_ltrb.bottom - dest_ltrb.top),
                                  .format = buffer_format},
                 .fb = std::make_shared<MockFbIdHandle>()},
      .composition_type = type,
      .display_frame = DstRectInfo{.i_rect = dest_ltrb},
      .alpha = alpha,
      .z_order = z_order,
  };
  layer.SetLayerProperties(props);

  return layer;
}
}  // namespace android::drm_hwcomposer