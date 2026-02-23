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

#include "DisplayRefreshRatesChangedAtomReporter.h"

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

const std::string kStatsServiceName = std::string(IStats::descriptor)
                                          .append("/default");

// Use a private implementation of DisplayRefreshRatesChangedAtomReporter to
// avoid leaking the IStats interface through the public api.
class DisplayRefreshRatesChangedAtomReporterDesktop
    : public DisplayRefreshRatesChangedAtomReporter {
 public:
  void UpdateRefreshRates(std::vector<int32_t> refresh_rates) override {
    if (refresh_rates == last_refresh_rates_) {
      return;
    }

    last_refresh_rates_ = refresh_rates;

    // The order of the arguments to createVendorAtom is determined by the
    // proto definition in libdesktopatoms.
    const char* deprecated_reverse_domain_name = "";
    const VendorAtom vendor_atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::DISPLAY_REFRESH_RATES_CHANGED,
                         deprecated_reverse_domain_name, refresh_rates);

    auto stats_service = IStats::fromBinder(ndk::SpAIBinder(
        AServiceManager_checkService(kStatsServiceName.c_str())));
    ALOGE_IF(stats_service == nullptr, "Failed to get IStats service");
    if (stats_service) {
      const ndk::ScopedAStatus ret = stats_service->reportVendorAtom(
          vendor_atom);
    }
  }

 private:
  std::vector<int32_t> last_refresh_rates_;
};
}  // namespace

std::unique_ptr<DisplayRefreshRatesChangedAtomReporter>
DisplayRefreshRatesChangedAtomReporter::Create() {
  if (!AServiceManager_isDeclared(kStatsServiceName.c_str())) {
    ALOGW("Stats service is not declared.");
    return nullptr;
  }
  return std::make_unique<DisplayRefreshRatesChangedAtomReporterDesktop>();
}

}  // namespace android::drm_hwcomposer
