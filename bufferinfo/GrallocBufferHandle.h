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

#include "bufferinfo/BufferInfo.h"

#include <cutils/native_handle.h>

#include <memory>

namespace android::drm_hwcomposer {

// GrallocBufferHandle manages the lifetime of a buffer_handle_t that has been
// imported into this process. Its lifetime is expected to be tracked by a
// shared_ptr<PrimeFdsSharedBase> such that the imported buffer_handle_t can be
// freed after there are no more BufferInfos using it.
class GrallocBufferHandle : public PrimeFdsSharedBase {
 public:
  static auto Create(buffer_handle_t handle)
      -> std::shared_ptr<GrallocBufferHandle>;

  auto GetHandle() const -> buffer_handle_t {
    return imported_handle_;
  }

  ~GrallocBufferHandle() override;

 protected:
  explicit GrallocBufferHandle(buffer_handle_t buffer_handle)
      : imported_handle_(buffer_handle) {
  }

 private:
  buffer_handle_t imported_handle_{};
};

}  // namespace android::drm_hwcomposer