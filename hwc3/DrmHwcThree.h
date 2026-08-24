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

#include <aidl/android/hardware/graphics/composer3/IComposerCallback.h>
#include <android-base/thread_annotations.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "compositor/DisplayInfo.h"
#include "drm/DrmHwc.h"
#include "hwc/HwcDisplay.h"

namespace aidl::android::hardware::graphics::composer3::impl {

class Hwc3Display : public ::android::drm_hwcomposer::FrontendDisplayBase {
 public:
  // Desired present time for a composition that has been validated but not
  // yet presented. nullopt means it should be presented at the next vsync.
  std::optional<int64_t> desired_present_time = std::nullopt;

  int64_t next_layer_id = 1;
};

class DrmHwcThree : public ::android::drm_hwcomposer::DrmHwc {
 public:
  explicit DrmHwcThree() = default;
  ~DrmHwcThree() override;

  void SetCallback(std::shared_ptr<IComposerCallback> callback);

  // DrmHwcInterface
  [[nodiscard]] bool HasCallback() const override {
    return composer_callback_ != nullptr;
  }
  void SendVsyncEventToClient(
      ::android::drm_hwcomposer::DisplayHandle display_handle,
      int64_t timestamp, uint32_t vsync_period) const override;
  void SendVsyncPeriodTimingChangedEventToClient(
      ::android::drm_hwcomposer::DisplayHandle display_handle,
      int64_t timestamp) const override;
  void SendRefreshEventToClient(
      ::android::drm_hwcomposer::DisplayHandle display_handle) override;
  void SendHotplugEventToClient(
      ::android::drm_hwcomposer::DisplayHandle display_handle,
      DrmHwc::DisplayStatus display_status) override;
  void SendHdcpLevelsChangedEventToClient(
      ::android::drm_hwcomposer::DisplayHandle display_handle,
      std::optional<enum ::android::drm_hwcomposer::HdcpContentType>
          current_hdcp_level) override;

  static auto GetHwc3Display(::android::drm_hwcomposer::HwcDisplay& display)
      -> std::shared_ptr<Hwc3Display>;

  auto GetMustValidateDisplay(
      ::android::drm_hwcomposer::DisplayHandle display_handle) -> bool;
  void ClearMustValidateDisplay(
      ::android::drm_hwcomposer::DisplayHandle display_handle);

 private:
  std::shared_ptr<IComposerCallback> composer_callback_;

  std::mutex must_validate_lock_;
  std::set<::android::drm_hwcomposer::DisplayHandle> must_validate_
      GUARDED_BY(must_validate_lock_);
};

}  // namespace aidl::android::hardware::graphics::composer3::impl
