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

#include <cutils/native_handle.h>

#include <memory>

#include "bufferinfo/GrallocBufferHandle.h"

namespace android::drm_hwcomposer {

// Stub for GrallocBufferHandle::Create
std::shared_ptr<GrallocBufferHandle> GrallocBufferHandle::Create(
    buffer_handle_t handle) {
  auto gralloc_handle = std::shared_ptr<GrallocBufferHandle>(
      new GrallocBufferHandle(handle));
  return gralloc_handle;
}

// Stub for GrallocBufferHandle::~GrallocBufferHandle
// Do not release the handle, since it wasn't imported.
GrallocBufferHandle::~GrallocBufferHandle() = default;

}  // namespace android::drm_hwcomposer
