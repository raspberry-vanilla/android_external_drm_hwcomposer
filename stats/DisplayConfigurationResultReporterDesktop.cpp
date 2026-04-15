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

#include <memory>
#include <string>

#include "stats/DisplayConfigurationResultReporter.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android::drm_hwcomposer {
namespace {

const std::string kStatsServiceName = std::string(IStats::descriptor)
                                          .append("/default");

DesktopAtoms::DisplayConfigurationResult::DisplayType ToProtoEnum(
    DisplayConfigurationResultReporter::DisplayType type) {
  switch (type) {
    case DisplayConfigurationResultReporter::DisplayType::kUnspecified:
      return DesktopAtoms::DisplayConfigurationResult::DISPLAY_TYPE_UNSPECIFIED;
    case DisplayConfigurationResultReporter::DisplayType::kInternal:
      return DesktopAtoms::DisplayConfigurationResult::DISPLAY_TYPE_INTERNAL;
    case DisplayConfigurationResultReporter::DisplayType::kExternal:
      return DesktopAtoms::DisplayConfigurationResult::DISPLAY_TYPE_EXTERNAL;
  }
}

class DisplayConfigurationResultReporterDesktop
    : public DisplayConfigurationResultReporter {
 public:
  void PushAtom(const DisplayConfigurationResultReporter::Atom& atom) override {
    const char* kDeprecatedReverseDomainName = "";
    const VendorAtom vendor_atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::DISPLAY_CONFIGURATION_RESULT,
                         kDeprecatedReverseDomainName, atom.display_handle,
                         atom.success, atom.is_seamless,
                         ToProtoEnum(atom.display_type));

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

std::unique_ptr<DisplayConfigurationResultReporter>
DisplayConfigurationResultReporter::Create() {
  if (!AServiceManager_isDeclared(kStatsServiceName.c_str())) {
    return nullptr;
  }
  return std::make_unique<DisplayConfigurationResultReporterDesktop>();
}

}  // namespace android::drm_hwcomposer
