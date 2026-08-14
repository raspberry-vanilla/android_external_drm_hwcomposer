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

// #define NLOG_DEBUG 0

#include <aidl/android/frameworks/stats/IStats.h>
#include <android/binder_auto_utils.h>
#include <android/binder_manager.h>
#include <desktopatoms.h>
#include <drm/drm_mode.h>
#include <ui/GraphicTypes.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stats/DisplayHotplugConnectModeDetectedAtomReporter.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android::drm_hwcomposer {
namespace {

DesktopAtoms::DisplayHotplugConnectModeDetected::DisplayType
DisplayTypeToProtoEnum(
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

DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType
ConnectionTypeToProtoEnum(uint32_t type, bool has_path) {
  switch (type) {
    case DRM_MODE_CONNECTOR_Unknown:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_UNSPECIFIED;
    case DRM_MODE_CONNECTOR_VGA:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_VGA;
    case DRM_MODE_CONNECTOR_DVII:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_DVII;
    case DRM_MODE_CONNECTOR_DVID:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_DVID;
    case DRM_MODE_CONNECTOR_DVIA:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_DVIA;
    case DRM_MODE_CONNECTOR_Composite:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_COMPOSITE;
    case DRM_MODE_CONNECTOR_SVIDEO:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_SVIDEO;
    case DRM_MODE_CONNECTOR_LVDS:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_LVDS;
    case DRM_MODE_CONNECTOR_Component:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_COMPONENT;
    case DRM_MODE_CONNECTOR_9PinDIN:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_9_PIN_DIN;
    case DRM_MODE_CONNECTOR_DisplayPort:
      return has_path ? DesktopAtoms::DisplayHotplugConnectModeDetected::
                            ConnectionType::CONNECTION_TYPE_DP_MST
                      : DesktopAtoms::DisplayHotplugConnectModeDetected::
                            ConnectionType::CONNECTION_TYPE_DP_SST;
    case DRM_MODE_CONNECTOR_HDMIA:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_HDMIA;
    case DRM_MODE_CONNECTOR_HDMIB:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_HDMIB;
    case DRM_MODE_CONNECTOR_TV:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_TV;
    case DRM_MODE_CONNECTOR_eDP:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_EDP;
    case DRM_MODE_CONNECTOR_VIRTUAL:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_VIRTUAL;
    case DRM_MODE_CONNECTOR_DSI:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_DSI;
    case DRM_MODE_CONNECTOR_DPI:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_DPI;
    case DRM_MODE_CONNECTOR_WRITEBACK:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_WRITEBACK;
    case DRM_MODE_CONNECTOR_SPI:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_SPI;
    case DRM_MODE_CONNECTOR_USB:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_USB;
    default:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::ConnectionType::
          CONNECTION_TYPE_UNSPECIFIED;
  }
}

DesktopAtoms::DisplayHotplugConnectModeDetected::HdrType HdrTypeToProtoEnum(
    ui::Hdr hdr) {
  switch (hdr) {
    case ui::Hdr::INVALID:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          HDR_TYPE_UNSPECIFIED;
    case ui::Hdr::DOLBY_VISION:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          HDR_TYPE_DOLBY_VISION;
    case ui::Hdr::HDR10:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::HDR_TYPE_HDR10;
    case ui::Hdr::HLG:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::HDR_TYPE_HLG;
    case ui::Hdr::HDR10_PLUS:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          HDR_TYPE_HDR10_PLUS;
    case ui::Hdr::DOLBY_VISION_4K30:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::
          HDR_TYPE_DOLBY_VISION_4K30;
    case ui::Hdr::HLG_PLUS:
      return DesktopAtoms::DisplayHotplugConnectModeDetected::HDR_TYPE_HLG_PLUS;
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
    static constexpr const char* kDeprecatedReverseDomainName = "";

    std::vector<int32_t> hdr_types;
    hdr_types.reserve(atom.hdr_types.size());
    for (const auto hdr_type : atom.hdr_types) {
      hdr_types.push_back(HdrTypeToProtoEnum(hdr_type));
    }

    const VendorAtom vendor_atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::DISPLAY_HOTPLUG_CONNECT_MODE_DETECTED,
                         kDeprecatedReverseDomainName, atom.display_handle,
                         atom.resolution_x, atom.resolution_y,
                         atom.refresh_rate, atom.dpi_x, atom.dpi_y,
                         DisplayTypeToProtoEnum(atom.display_type),
                         atom.is_preferred, atom.make.c_str(),
                         atom.model.c_str(), atom.year, hdr_types,
                         atom.max_luminance, atom.max_average_luminance,
                         atom.min_luminance,
                         ConnectionTypeToProtoEnum(atom.connection_type,
                                                   atom.has_path),
                         atom.vrr_range_min, atom.vrr_range_max);

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
