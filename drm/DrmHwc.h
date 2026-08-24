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

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "drm/ResourceManager.h"
#include "hwc/HwcDisplay.h"
#include "stats/DisplayRefreshRatesChangedAtomReporter.h"
#include "stats/Stats.h"

namespace android::drm_hwcomposer {

struct DrmDisplayPipeline;
class EarlyBootAnimation;

class DrmHwc : public PipelineToFrontendBindingInterface, public StatsProvider {
 public:
  DrmHwc();
  ~DrmHwc() override;

  // Enum for Display status: Connected, Disconnected, Link Training Failed
  enum DisplayStatus {
    kDisconnected,
    kConnected,
    kLinkTrainingFailed,
  };

  // Client Callback functions.:
  // Check if an active client callback is registered for event delivery.
  [[nodiscard]] virtual bool HasCallback() const = 0;
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

  // StatsProvider:
  auto PullCompositionStats()
      -> std::map<CompositionAttributes, CompositionStats> override;
  auto PullActiveDisplayCounts() -> ActiveDisplayCounts override;

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
    hotplug_event_queue_.Add(display_handle, display_status);
  }

  void DeinitDisplays();

  // PipelineToFrontendBindingInterface
  bool BindDisplay(std::shared_ptr<DrmDisplayPipeline> pipeline) override;
  bool UnbindDisplay(std::shared_ptr<DrmDisplayPipeline> pipeline) override;
  void FinalizeDisplayBinding() override;
  void FlushHotplugEvents() override;

  // Notify Display Link Status
  void NotifyDisplayLinkStatus(
      std::shared_ptr<DrmDisplayPipeline> pipeline) override;

  // Notify HDCP Termination from kernel Uevents
  void NotifyHdcpTermination(
      std::shared_ptr<DrmDisplayPipeline> pipeline) override;

  // Should be done for all successful modesets (full and seamless).
  void LogRefreshRateChanges();

  void StartBootAnimation();
  void WaitForCompletionAndStopBootAnimation();
  void StopBootAnimation();

 protected:
  auto &Displays() {
    return displays_;
  }

 private:
  // An additional layer to isolate the critical region for invoking hotplug
  // event callbacks from the main mutex.
  class HotplugEventQueue {
   public:
    using Events = std::map<DisplayHandle, DisplayStatus>;

    void Add(DisplayHandle display_handle, DisplayStatus display_status);
    Events RetrieveAndFlush();

   private:
    Events events_;
    std::mutex mutex_;
  };

  ResourceManager resource_manager_;
  std::map<DisplayHandle, std::unique_ptr<HwcDisplay>> displays_;
  std::map<std::shared_ptr<DrmDisplayPipeline>, DisplayHandle> display_handles_;

  HotplugEventQueue hotplug_event_queue_;
  std::vector<DisplayHandle> displays_for_removal_list_;

  void RequestHdcpNegotiation(DisplayHandle display_handle);

  DisplayHandle last_display_handle_ = kPrimaryDisplay;
  StatsTracker dump_stats_tracker_;

  std::unique_ptr<DisplayRefreshRatesChangedAtomReporter>
      refresh_rates_reporter_;

  bool hdcp_on_hotplug_enabled_{};
  std::unique_ptr<EarlyBootAnimation> boot_animation_;
};

}  // namespace android::drm_hwcomposer
