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

#include <cstdint>
#include <memory>
#include <string>

#include "stats/CountActiveDisplaysReporter.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android::drm_hwcomposer {
namespace {

const std::string kStatsServiceName = std::string(IStats::descriptor)
                                          .append("/default");

class CompositionStatsReporterDesktop : public CountActiveDisplaysReporter {
 public:
  void PushAtom(int32_t num_active_physical_displays,
                int32_t num_active_external_displays,
                int32_t num_virtual_displays) override {
    const char* kDeprecatedReverseDomainName = "";
    const VendorAtom atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::COUNT_ACTIVE_DISPLAYS,
                         kDeprecatedReverseDomainName,
                         num_active_physical_displays,
                         num_active_external_displays, num_virtual_displays);

    auto stats_service = IStats::fromBinder(ndk::SpAIBinder(
        AServiceManager_checkService(kStatsServiceName.c_str())));
    ALOGE_IF(stats_service == nullptr, "Failed to get IStats service");
    if (stats_service) {
      const ndk::ScopedAStatus ret = stats_service->reportVendorAtom(atom);
      ALOGE_IF(!ret.isOk(), "Failed to report stats: %s",
               ret.getDescription().c_str());
    }
  }
};
}  // namespace

std::unique_ptr<CountActiveDisplaysReporter>
CountActiveDisplaysReporter::Create() {
  if (!AServiceManager_isDeclared(kStatsServiceName.c_str())) {
    return nullptr;
  }
  return std::make_unique<CompositionStatsReporterDesktop>();
}

}  // namespace android::drm_hwcomposer
