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

#include "DrmHwc.h"

#include <cinttypes>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

#include "stats/CompositionStats.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {
// Helper functions for implementing dumpsys support.
std::string DumpStats(const CompositionStats &stats) {
  if (stats.total_pixops == 0)
    return "No stats yet";

  // NOLINTNEXTLINE(readability-magic-numbers)
  auto ratio = 1.0 - (double(stats.gpu_pixops) / double(stats.total_pixops));

  std::stringstream ss;
  ss << " Total frames count: " << stats.total_frames << "\n"
     << " Failed cursor test commit frames: "
     << stats.failed_kms_cursor_validate << "\n"
     << " Failed to test commit frames: " << stats.failed_kms_validate << "\n"
     << " Failed to commit frames: " << stats.failed_kms_present << "\n"
     << ((stats.failed_kms_present > 0)
             ? " !!! Internal failure, FIX it please\n"
             : "")
     << " Flattened frames: " << stats.frames_flattened << "\n"
     << " Cursor plane frames: " << stats.cursor_plane_frames << "\n"
     << " Pixel operations (free units) : [TOTAL: " << stats.total_pixops
     << " / GPU: " << stats.gpu_pixops << "]\n"
     << " Composition efficiency: " << ratio;
  return ss.str();
}

std::string DumpDisplayStats(const HwcDisplay *display,
                             const CompositionStats &stats,
                             const CompositionStats &delta) {
  std::stringstream ss;
  ss << "- Display on: " << display->GetDisplayName() << "\n"
     << "Statistics since system boot:\n"
     << DumpStats(stats) << "\n\n"
     << "Statistics since last dumpsys request:\n"
     << DumpStats(delta) << "\n\n";
  return ss.str();
}
}  // namespace

DrmHwc::DrmHwc() : resource_manager_(this), dump_stats_tracker_(this) {};

/* Must be called after every display attach/detach cycle */
void DrmHwc::FinalizeDisplayBinding() {
  if (displays_.count(kPrimaryDisplay) == 0) {
    /* Primary display MUST always exist */
    ALOGI("No pipelines available. Creating null-display for headless mode");
    displays_[kPrimaryDisplay] = std::make_unique<
        HwcDisplay>(kPrimaryDisplay, /* is_virtual */ false, this);
    /* Initializes null-display */
    displays_[kPrimaryDisplay]->SetPipeline({});
  }

  if (displays_[kPrimaryDisplay]->IsInHeadlessMode() &&
      !display_handles_.empty()) {
    /* Reattach first secondary display to take place of the primary */
    auto pipe = display_handles_.begin()->first;
    ALOGI("Primary display was disconnected, reattaching '%s' as new primary",
          pipe->connector->Get()->GetName().c_str());
    UnbindDisplay(pipe);
    BindDisplay(pipe);
  }

  // Finally, send hotplug events to the client
  for (auto &dhe : deferred_hotplug_events_) {
    SendHotplugEventToClient(dhe.first, dhe.second);
  }
  deferred_hotplug_events_.clear();

  for (auto handle : displays_for_removal_list_) {
    displays_.erase(handle);
  }
  displays_for_removal_list_.clear();
}

bool DrmHwc::BindDisplay(std::shared_ptr<DrmDisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) != 0) {
    ALOGE("%s, pipeline is already used by another display, FIXME!!!: %p",
          __func__, pipeline.get());
    return false;
  }

  uint32_t disp_handle = kPrimaryDisplay;

  if (displays_.count(kPrimaryDisplay) != 0 &&
      !displays_[kPrimaryDisplay]->IsInHeadlessMode()) {
    disp_handle = ++last_display_handle_;
  }

  if (displays_.count(disp_handle) == 0) {
    auto disp = std::make_unique<HwcDisplay>(disp_handle,
                                             /* is_virtual */ false, this);
    displays_[disp_handle] = std::move(disp);
  }

  ALOGI("Attaching pipeline '%s' to the display #%d%s",
        pipeline->connector->Get()->GetName().c_str(), (int)disp_handle,
        disp_handle == kPrimaryDisplay ? " (Primary)" : "");

  displays_[disp_handle]->SetPipeline(pipeline);
  display_handles_[pipeline] = disp_handle;

  return true;
}

bool DrmHwc::UnbindDisplay(std::shared_ptr<DrmDisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) == 0) {
    ALOGE("%s, can't find the display, pipeline: %p", __func__, pipeline.get());
    return false;
  }
  auto handle = display_handles_[pipeline];
  display_handles_.erase(pipeline);

  ALOGI("Detaching the pipeline '%s' from the display #%i%s",
        pipeline->connector->Get()->GetName().c_str(), (int)handle,
        handle == kPrimaryDisplay ? " (Primary)" : "");

  if (displays_.count(handle) == 0) {
    ALOGE("%s, can't find the display, handle: %" PRIu64, __func__, handle);
    return false;
  }
  displays_[handle]->SetPipeline({});

  /* Defer destruction of the display until after the hotplug event is sent. */
  if (handle != kPrimaryDisplay) {
    displays_for_removal_list_.emplace_back(handle);
  }
  return true;
}

void DrmHwc::NotifyDisplayLinkStatus(
    std::shared_ptr<DrmDisplayPipeline> pipeline) {
  if (display_handles_.count(pipeline) == 0) {
    ALOGE("%s, can't find the display, pipeline: %p", __func__, pipeline.get());
    return;
  }
  ScheduleHotplugEvent(display_handles_[pipeline],
                       DisplayStatus::kLinkTrainingFailed);
}

std::optional<DisplayHandle> DrmHwc::CreateVirtualDisplay(uint32_t width,
                                                          uint32_t height) {
  ALOGI("Creating virtual display %dx%d", width, height);

  auto virtual_pipeline = resource_manager_.GetVirtualDisplayPipeline();
  if (!virtual_pipeline)
    return std::nullopt;

  DisplayHandle new_display_handle = ++last_display_handle_;
  auto disp = std::make_unique<HwcDisplay>(new_display_handle,
                                           /* is_virtual */ true, this);

  disp->SetVirtualDisplayResolution(width, height);
  disp->SetPipeline(virtual_pipeline);
  displays_[new_display_handle] = std::move(disp);
  return new_display_handle;
}

bool DrmHwc::DestroyVirtualDisplay(DisplayHandle display) {
  ALOGI("Destroying virtual display %" PRIu64, display);

  if (displays_.count(display) == 0) {
    ALOGE("Trying to destroy non-existent display %" PRIu64, display);
    return false;
  }

  if (displays_[display]->GetDisplayType() !=
      HwcDisplay::DisplayType::kVirtual) {
    ALOGE("Trying to destroy non-virtual display %" PRIu64, display);
    return false;
  }

  displays_[display]->SetPipeline({});
  displays_.erase(display);
  return true;
}

auto DrmHwc::PullCompositionStats()
    -> std::map<CompositionAttributes, CompositionStats> {
  std::map<CompositionAttributes, CompositionStats> stats;
  for (const auto &[display_handle, display] : displays_) {
    stats.insert(display->comp_stats().begin(), display->comp_stats().end());
  }
  return stats;
}

std::string DrmHwc::DumpState() {
  std::stringstream output;

  output << "-- drm_hwcomposer --\n\n";

  std::map<DisplayHandle, std::pair<CompositionStats, CompositionStats>>
      total_stats;
  const auto callback = [&total_stats](const CompositionAttributes &attributes,
                                       const CompositionStats &cumulative,
                                       const CompositionStats &delta) {
    auto it = total_stats.find(attributes.display_handle);
    if (it == total_stats.end()) {
      total_stats.emplace(attributes.display_handle,
                          std::make_pair(CompositionStats{},
                                         CompositionStats{}));
      it = total_stats.find(attributes.display_handle);
    }
    auto &[total_cumulative, total_delta] = it->second;
    total_cumulative += cumulative;
    total_delta += delta;
  };
  dump_stats_tracker_.ReportStats(callback);

  for (const auto &[display_handle, display_stats] : total_stats) {
    const auto *display = GetDisplay(display_handle);
    ALOGE_IF(display == nullptr, "Display %" PRIu64 " not found",
             display_handle);
    if (display != nullptr) {
      output << DumpDisplayStats(display, display_stats.first,
                                 display_stats.second);
    }
  }
  return output.str();
}

uint32_t DrmHwc::GetMaxVirtualDisplayCount() {
  /* Virtual display is an experimental feature.
   * Unless explicitly set to true, return 0 for no support.
   */
  if (!Properties::EnableVirtualDisplay()) {
    return 0;
  }

  auto writeback_count = resource_manager_.GetWritebackConnectorsCount();
  writeback_count = std::min(writeback_count, 1U);
  /* Currently, only 1 virtual display is supported. Other cases need testing */
  ALOGI("Max virtual display count: %d", writeback_count);
  return writeback_count;
}

void DrmHwc::DeinitDisplays() {
  for (auto &pair : Displays()) {
    pair.second->SetPipeline(nullptr);
  }
}

}  // namespace android::drm_hwcomposer
