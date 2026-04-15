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

#include <cutils/native_handle.h>

#include <cstdint>
#include <optional>

#include "hwc/HwcBufferCache.h"
#include "hwc/HwcLayer.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

// Imports and caches gralloc buffers based on the behavior specified in HWC3
// documentation. The gralloc buffers will be imported as a GrallocBufferHandle,
// with its associated BufferInfo holding a handle to the GrallocBufferHandle.
class GrallocBufferCache : public FrontendLayerBase {
 public:
  // |importer| is the callback used to import drm framebuffers.
  explicit GrallocBufferCache(HwcBufferCache::ImporterCallback importer);

  // If |raw_handle| is std::nullopt, use the cached buffer. If |raw_handle| is
  // set, update the cache. In both cases, return a HwcLayer::Buffer that can be
  // used to populated the HwcLayer::LayerProperties.
  auto HandleNextBuffer(std::optional<buffer_handle_t> raw_handle,
                        SharedFd fence_fd, int32_t slot_id)
      -> std::optional<HwcLayer::Buffer>;

  void ClearSlot(int32_t slot_id);

  void ClearSlots();

 private:
  HwcBufferCache buffer_cache_;
};

}  // namespace android::drm_hwcomposer