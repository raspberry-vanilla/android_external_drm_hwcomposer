/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "compositor/DisplayInfo.h"
#include "drm/ResourceManager.h"
#include "hwc/HwcDisplay.h"
#include "stats/CompositionStats.h"
#include "stats/DisplayRefreshRatesChangedAtomReporter.h"

namespace android::drm_hwcomposer {

struct DrmDisplayPipeline;

class DrmHwc : public PipelineToFrontendBindingInterface,
               public CompositionStatsProvider {
 public:
  DrmHwc();
  ~DrmHwc() override = default;

  // Enum for Display status: Connected, Disconnected, Link Training Failed
  enum DisplayStatus {
    kDisconnected,
    kConnected,
    kLinkTrainingFailed,
  };

  // Client Callback functions.:
  virtual void SendVsyncEventToClient(DisplayHandle display_handle,
                                      int64_t timestamp,
                                      uint32_t vsync_period) const = 0;
  virtual void SendVsyncPeriodTimingChangedEventToClient(
      DisplayHandle display_handle, int64_t timestamp) const = 0;
  virtual void SendRefreshEventToClient(DisplayHandle display_handle) = 0;
  virtual void SendHotplugEventToClient(DisplayHandle display_handle,
                                        enum DisplayStatus display_status) = 0;
  virtual void SendHdcpLevelsChangedEventToClient(
      DisplayHandle display_handle,
      std::optional<enum HdcpContentType> current_hdcp_level) = 0;

  // CompositionStatsProvider:
  auto PullCompositionStats()
      -> std::map<CompositionAttributes, CompositionStats> override;

  std::string DumpState();

  // Virtual Display functions.
  std::optional<DisplayHandle> CreateVirtualDisplay(uint32_t width,
                                                    uint32_t height);
  bool DestroyVirtualDisplay(DisplayHandle display_handle);
  uint32_t GetMaxVirtualDisplayCount();

  auto GetDisplay(DisplayHandle display_handle) {
    return displays_.count(display_handle) != 0
               ? displays_[display_handle].get()
               : nullptr;
  }

  auto &GetResMan() {
    return resource_manager_;
  }

  void ScheduleHotplugEvent(DisplayHandle display_handle,
                            enum DisplayStatus display_status) {
    deferred_hotplug_events_[display_handle] = display_status;
  }

  void DeinitDisplays();

  // PipelineToFrontendBindingInterface
  bool BindDisplay(std::shared_ptr<DrmDisplayPipeline> pipeline) override;
  bool UnbindDisplay(std::shared_ptr<DrmDisplayPipeline> pipeline) override;
  void FinalizeDisplayBinding() override;

  // Notify Display Link Status
  void NotifyDisplayLinkStatus(
      std::shared_ptr<DrmDisplayPipeline> pipeline) override;

  // Should be done for all successful modesets (full and seamless).
  void LogRefreshRateChanges();

 protected:
  auto &Displays() {
    return displays_;
  }

 private:
  ResourceManager resource_manager_;
  std::map<DisplayHandle, std::unique_ptr<HwcDisplay>> displays_;
  std::map<std::shared_ptr<DrmDisplayPipeline>, DisplayHandle> display_handles_;

  std::map<DisplayHandle, enum DisplayStatus> deferred_hotplug_events_;
  std::vector<DisplayHandle> displays_for_removal_list_;

  DisplayHandle last_display_handle_ = kPrimaryDisplay;
  CompositionStatsTracker dump_stats_tracker_;

  std::unique_ptr<DisplayRefreshRatesChangedAtomReporter>
      refresh_rates_reporter_;
};

}  // namespace android::drm_hwcomposer
