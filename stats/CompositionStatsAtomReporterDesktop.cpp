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

#include "CompositionStatsAtomReporter.h"

#include <cinttypes>
#include <thread>

#include <aidl/android/frameworks/stats/IStats.h>
#include <android/binder_manager.h>

#include "desktopatoms.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android {
namespace {

// Use a private implementation of CompositionStatsAtomReporter to avoid leaking
// the IStats interface through the public api.
class CompositionStatsReporterDesktop : public CompositionStatsAtomReporter {
 public:
  explicit CompositionStatsReporterDesktop(std::shared_ptr<IStats> stats_client)
      : stats_client_(std::move(stats_client)) {
  }

  void PushAtom(int64_t display_handle, int64_t presented_frame_count,
                int64_t present_failed_count,
                int64_t validate_failed_count) override {
    ALOGV("Sending stats: id=%" PRId64 ", frames=%" PRId64
          ", failed_present=%" PRId64 ", failed_validate=%" PRId64,
          display_handle, presented_frame_count, present_failed_count,
          validate_failed_count);
    // The order of the arguments to createVendorAtom is determined by the
    // proto definition in libdesktopatoms.
    const char* kDeprecatedReverseDomainName = "";
    const VendorAtom atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::HWC_COMPOSITION_STATS,
                         kDeprecatedReverseDomainName, display_handle,
                         presented_frame_count, present_failed_count,
                         validate_failed_count);
    const ndk::ScopedAStatus ret = stats_client_->reportVendorAtom(atom);
    ALOGE_IF(!ret.isOk(), "Failed to report stats: %s",
             ret.getDescription().c_str());
  }

 private:
  std::shared_ptr<IStats> stats_client_;
};

auto GetStatsService() -> std::shared_ptr<IStats> {
  const std::string stats_service_name = std::string(IStats::descriptor)
                                             .append("/default");
  if (!AServiceManager_isDeclared(stats_service_name.c_str())) {
    ALOGW("Stats service is not declared.");
    return nullptr;
  }
  return IStats::fromBinder(ndk::SpAIBinder(
      AServiceManager_waitForService(stats_service_name.c_str())));
}

}  // namespace

std::unique_ptr<CompositionStatsAtomReporter>
CompositionStatsAtomReporter::Create() {
  std::shared_ptr<IStats> stats_client = GetStatsService();
  ALOGW_IF(!stats_client, "Failed to get stats service");
  if (!stats_client) {
    return {};
  }
  return std::make_unique<CompositionStatsReporterDesktop>(stats_client);
}

}  // namespace android