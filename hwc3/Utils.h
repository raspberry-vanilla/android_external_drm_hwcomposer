/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <aidl/android/hardware/graphics/composer3/IComposerClient.h>
#include <android/binder_auto_utils.h>

#include <cstdint>

#include "utils/log.h"

// NOLINTNEXTLINE
#define DEBUG_FUNC() ALOGV("%s", __func__)

namespace aidl::android::hardware::graphics::composer3 {

namespace hwc3 {
enum class Error : int32_t {
  kNone = 0,
  kBadConfig = IComposerClient::EX_BAD_CONFIG,
  kBadDisplay = IComposerClient::EX_BAD_DISPLAY,
  kBadLayer = IComposerClient::EX_BAD_LAYER,
  kBadParameter = IComposerClient::EX_BAD_PARAMETER,
  kNoResources = IComposerClient::EX_NO_RESOURCES,
  kNotValidated = IComposerClient::EX_NOT_VALIDATED,
  kUnsupported = IComposerClient::EX_UNSUPPORTED,
  kSeamlessNotAllowed = IComposerClient::EX_SEAMLESS_NOT_ALLOWED,
  kSeamlessNotPossible = IComposerClient::EX_SEAMLESS_NOT_POSSIBLE,
  #if __ANDROID_API__ >= 36
  kConfigFailed = IComposerClient::EX_CONFIG_FAILED,
  #endif
};
}  // namespace hwc3

inline ndk::ScopedAStatus ToBinderStatus(hwc3::Error error) {
  if (error != hwc3::Error::kNone) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(error));
  }
  return ndk::ScopedAStatus::ok();
}

};  // namespace aidl::android::hardware::graphics::composer3
