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

#define LOG_TAG "drmhwc"
// #define NLOG_DEBUG 0

#include "DisplayHotplugConnectModeDetectedAtomReporter.h"

#include <cinttypes>
#include <thread>

#include <aidl/android/frameworks/stats/IStats.h>
#include <android/binder_manager.h>

#include "desktopatoms.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android::drm_hwcomposer {
namespace {

DesktopAtoms::DisplayHotplugConnectModeDetected::DisplayType ToProtoEnum(
    DisplayHotplugConnectModeDetectedAtomReporter::DisplayType type) {
  switch (type) {
    case DisplayHotplugConnectModeDetectedAtomReporter::DisplayType::
        kUnspecified:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          DISPLAY_TYPE_UNSPECIFIED;
    case DisplayHotplugConnectModeDetectedAtomReporter::DisplayType::kInternal:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          DISPLAY_TYPE_INTERNAL;
    case DisplayHotplugConnectModeDetectedAtomReporter::DisplayType::kExternal:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          DISPLAY_TYPE_EXTERNAL;
  }
}

const std::string kStatsServiceName = std::string(IStats::descriptor)
                                          .append("/default");

// Use a private implementation of DisplayHotplugConnectModeDetectedAtomReporter
// to avoid leaking the IStats interface through the public api.
class DisplayHotplugConnectModeDetectedAtomReporterDesktop
    : public DisplayHotplugConnectModeDetectedAtomReporter {
 public:
  void PushAtom(Atom atom) override {
    // The order of the arguments to createVendorAtom is determined by the
    // proto definition in libdesktopatoms.
    const char* deprecated_reverse_domain_name = "";
    const VendorAtom vendor_atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::DISPLAY_HOTPLUG_CONNECT_MODE_DETECTED,
                         deprecated_reverse_domain_name, atom.display_handle,
                         atom.resolution_x, atom.resolution_y,
                         atom.refresh_rate, atom.dpi_x, atom.dpi_y,
                         ToProtoEnum(atom.display_type), atom.is_preferred);

    auto stats_service = IStats::fromBinder(ndk::SpAIBinder(
        AServiceManager_checkService(kStatsServiceName.c_str())));
    ALOGE_IF(stats_service == nullptr, "Failed to get IStats service");
    if (stats_service) {
      const ndk::ScopedAStatus ret = stats_service->reportVendorAtom(
          vendor_atom);
      ALOGE_IF(!ret.isOk(), "Failed to report stats: %s",
               ret.getDescription().c_str());
    }
  }
};
}  // namespace

std::unique_ptr<DisplayHotplugConnectModeDetectedAtomReporter>
DisplayHotplugConnectModeDetectedAtomReporter::Create() {
  if (!AServiceManager_isDeclared(kStatsServiceName.c_str())) {
    ALOGW("Stats service is not declared.");
    return nullptr;
  }
  return std::make_unique<
      DisplayHotplugConnectModeDetectedAtomReporterDesktop>();
}

}  // namespace android::drm_hwcomposer
