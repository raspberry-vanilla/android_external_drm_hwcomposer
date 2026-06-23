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

#include <chrono>
#include <cstdint>
#include <memory>

#include "bufferinfo/BufferInfo.h"
#include "compositor/FrameTimeHistory.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

FrameTimeHistory CreateFrameTimeHistory(bool is_active) {
  FrameTimeHistory history;
  if (is_active) {
    const auto now = std::chrono::steady_clock::now();
    // 10 samples for 1000fps, should be active enough.
    for (int i = 10; i > 0; i--) {
      history.AddFrameTime(now - std::chrono::milliseconds(i));
    }
  }
  return history;
}
}  // namespace

HwcLayer CompositorTestUtils::CreateLayer(ICompositorDisplay* display,
                                          IRect dest_ltrb, uint32_t z_order,
                                          CompositionType type, float alpha,
                                          uint32_t buffer_format,
                                          bool is_active) {
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

  layer.layer_data_.frame_time_history = CreateFrameTimeHistory(is_active);

  return layer;
}
}  // namespace android::drm_hwcomposer