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

#include <cutils/native_handle.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Errors.h>

#include <memory>

#include "utils/log.h"

namespace android::drm_hwcomposer {

auto GrallocBufferHandle::Create(buffer_handle_t handle)
    -> std::shared_ptr<GrallocBufferHandle> {
  buffer_handle_t imported_handle{};
  auto result = ::android::GraphicBufferMapper::get()
                    .importBufferNoValidate(handle, &imported_handle);

  if (result != ::android::NO_ERROR) {
    ALOGE("Failed to import buffer handle: %d", result);
    return nullptr;
  }
  // Since GrallocBufferHandle c'tor is not public, we can't use
  // std::make_shared.
  return std::shared_ptr<GrallocBufferHandle>(
      new GrallocBufferHandle(imported_handle));
}

GrallocBufferHandle::~GrallocBufferHandle() {
  ::android::GraphicBufferMapper::get().freeBuffer(imported_handle_);
}

}  // namespace android::drm_hwcomposer