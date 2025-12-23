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

#include "bufferinfo/GrallocBufferHandle.h"

#include <ui/GraphicBufferMapper.h>

namespace android::drm_hwcomposer {

auto GrallocBufferHandle::Create(buffer_handle_t handle)
    -> std::shared_ptr<GrallocBufferHandle> {
  // Since GrallocBufferHandle c'tor is private, we can't use std::make_shared.
  auto hwc3 = std::shared_ptr<GrallocBufferHandle>(new GrallocBufferHandle());

  auto result = ::android::GraphicBufferMapper::get()
                    .importBufferNoValidate(handle, &hwc3->imported_handle_);

  if (result != ::android::NO_ERROR) {
    ALOGE("Failed to import buffer handle: %d", result);
    return nullptr;
  }

  return hwc3;
}

GrallocBufferHandle::~GrallocBufferHandle() {
  ::android::GraphicBufferMapper::get().freeBuffer(imported_handle_);
}

}  // namespace android::drm_hwcomposer