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

#include <cutils/native_handle.h>
#include <memory>

#include "bufferinfo/GrallocBufferHandle.h"

namespace sdm {
class DebugCallbackIntf;
}  // namespace sdm

namespace vendor::qti::hardware::display::snapalloc {
class SnapHandle;
}  // namespace vendor::qti::hardware::display::snapalloc

namespace android::drm_hwcomposer {

// Wrapper around a QTI SnapHandle. This should be used with the QTI gralloc
// implementation to ensure that gralloc buffers and associated metadata are
// imported for usage in SDM. SnapAllocHandle manages the lifetime of this
// reference to the underlying snapalloc::SnapHandle through the ISnapMapper
// interface.
class SnapAllocHandle : public GrallocBufferHandle {
 public:
  // This must be called before the first attempt to create a SnapAllocHandle.
  static bool Init(sdm::DebugCallbackIntf *debug_callback_intf);

  // Create a SnapAllocHandle from a buffer_handle_t. Import the buffer_handle_t
  // into this process.
  static std::shared_ptr<SnapAllocHandle> Create(buffer_handle_t handle);

  vendor::qti::hardware::display::snapalloc::SnapHandle *GetSnapHandle() const;
  ~SnapAllocHandle() override;

 private:
  SnapAllocHandle(
      buffer_handle_t buffer_handle,
      vendor::qti::hardware::display::snapalloc::SnapHandle *snap_handle);

  vendor::qti::hardware::display::snapalloc::SnapHandle *snap_handle_;
};

}  // namespace android::drm_hwcomposer
