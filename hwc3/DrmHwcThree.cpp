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

#define LOG_TAG "drmhwc"

#include "DrmHwcThree.h"

#include <cinttypes>

#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/DisplayHotplugEvent.h>

#include "drm/DrmHwc.h"
#include "hwc/HwcDisplay.h"

namespace aidl::android::hardware::graphics::composer3::impl {

auto DrmHwcThree::GetHwc3Display(::android::drm_hwcomposer::HwcDisplay& display)
    -> std::shared_ptr<Hwc3Display> {
  auto frontend_private_data = display.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<Hwc3Display>();
    display.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<Hwc3Display>(frontend_private_data);
}

DrmHwcThree::~DrmHwcThree() {
  /* Display deinit routine is handled by resource manager */
  GetResMan().DeInit();
}

void DrmHwcThree::Init(std::shared_ptr<IComposerCallback> callback) {
  composer_callback_ = std::move(callback);
  GetResMan().Init();
}

void DrmHwcThree::SendVsyncPeriodTimingChangedEventToClient(
    ::android::drm_hwcomposer::DisplayHandle display_handle,
    int64_t timestamp) const {
  VsyncPeriodChangeTimeline timeline;
  timeline.newVsyncAppliedTimeNanos = timestamp;
  timeline.refreshRequired = false;
  timeline.refreshTimeNanos = 0;

  composer_callback_->onVsyncPeriodTimingChanged(static_cast<int64_t>(
                                                     display_handle),
                                                 timeline);
}

void DrmHwcThree::SendRefreshEventToClient(
    ::android::drm_hwcomposer::DisplayHandle display_handle) {
  {
    const std::scoped_lock lock(must_validate_lock_);
    must_validate_.insert(display_handle);
  }
  composer_callback_->onRefresh(static_cast<int64_t>(display_handle));
}

void DrmHwcThree::SendVsyncEventToClient(
    ::android::drm_hwcomposer::DisplayHandle display_handle, int64_t timestamp,
    uint32_t vsync_period) const {
  composer_callback_->onVsync(static_cast<int64_t>(display_handle), timestamp,
                              static_cast<int32_t>(vsync_period));
}

void DrmHwcThree::SendHotplugEventToClient(
    ::android::drm_hwcomposer::DisplayHandle display_handle,
    DrmHwc::DisplayStatus display_status) {
  common::DisplayHotplugEvent event = common::DisplayHotplugEvent::DISCONNECTED;
  switch (display_status) {
    case DrmHwc::kDisconnected:
      event = common::DisplayHotplugEvent::DISCONNECTED;
      break;
    case DrmHwc::kConnected:
      event = common::DisplayHotplugEvent::CONNECTED;
      break;
    case DrmHwc::kLinkTrainingFailed:
      event = common::DisplayHotplugEvent::ERROR_INCOMPATIBLE_CABLE;
      break;
  }
  if (event == common::DisplayHotplugEvent::DISCONNECTED) {
    ClearMustValidateDisplay(display_handle);
  }
  composer_callback_->onHotplugEvent(static_cast<int64_t>(display_handle),
                                     event);
}

void DrmHwcThree::SendHdcpLevelsChangedEventToClient(
    ::android::drm_hwcomposer::DisplayHandle display_handle,
    std::optional<enum ::android::drm_hwcomposer::HdcpContentType>
        current_hdcp_level) {
  drm::HdcpLevels hdcplevel;
  // Set the maxLevel as set in SurfaceFlinger for Highest HDCP level
  hdcplevel.maxLevel = drm::HdcpLevel::HDCP_V2_3;

  if (!current_hdcp_level.has_value()) {
    hdcplevel.connectedLevel = drm::HdcpLevel::HDCP_NONE;
    composer_callback_->onHdcpLevelsChanged(static_cast<int64_t>(
                                                display_handle),
                                            hdcplevel);
    return;
  }
  switch (current_hdcp_level.value()) {
    case ::android::drm_hwcomposer::HdcpContentType::kType0:
      hdcplevel.connectedLevel = drm::HdcpLevel::HDCP_V1;
      break;
    case ::android::drm_hwcomposer::HdcpContentType::kType1:
      hdcplevel.connectedLevel = drm::HdcpLevel::HDCP_V2_2;
      break;
  }
  composer_callback_->onHdcpLevelsChanged(static_cast<int64_t>(display_handle),
                                          hdcplevel);
}

auto DrmHwcThree::GetMustValidateDisplay(
    ::android::drm_hwcomposer::DisplayHandle display_handle) -> bool {
  std::scoped_lock lock(must_validate_lock_);
  return must_validate_.find(display_handle) != must_validate_.end();
}

void DrmHwcThree::ClearMustValidateDisplay(
    ::android::drm_hwcomposer::DisplayHandle display_handle) {
  std::scoped_lock lock(must_validate_lock_);
  must_validate_.erase(display_handle);
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
