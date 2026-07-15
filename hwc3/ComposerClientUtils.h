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

#include <aidl/android/hardware/graphics/composer3/DisplayCommand.h>
#include <cutils/native_handle.h>

#include <memory>

#include "hwc/HwcDisplay.h"

// Forward declarations
namespace android::drm_hwcomposer {
struct BufferInfo;
class DrmFbIdHandle;
class GrallocBufferCache;
class HwcLayer;
}  // namespace android::drm_hwcomposer

namespace aidl::android::hardware::graphics::composer3 {
class CommandResultWriter;
}  // namespace aidl::android::hardware::graphics::composer3

namespace aidl::android::hardware::graphics::composer3::impl {

class DrmHwcThree;

struct NativeHandleDeleter {
  void operator()(const native_handle_t* h) const;
};

auto ImportFb(const ::android::drm_hwcomposer::HwcDisplay* display,
              ::android::drm_hwcomposer::BufferInfo& bi)
    -> std::shared_ptr<::android::drm_hwcomposer::DrmFbIdHandle>;

auto GetBufferCache(::android::drm_hwcomposer::HwcDisplay* parent,
                    ::android::drm_hwcomposer::HwcLayer& layer)
    -> std::shared_ptr<::android::drm_hwcomposer::GrallocBufferCache>;

void ExecuteDisplayCommand(DrmHwcThree& hwc, const DisplayCommand& command,
                           CommandResultWriter& cmd_result_writer);

}  // namespace aidl::android::hardware::graphics::composer3::impl
