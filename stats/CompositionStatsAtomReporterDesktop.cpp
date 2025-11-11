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

#include "compositor/CompositionPlanner.h"
#include "desktopatoms.h"
#include "stats/CompositionStats.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android::drm_hwcomposer {
namespace {

using FlattenReason = CompositionPlanner::FlattenReason;

const std::string kStatsServiceName = std::string(IStats::descriptor)
                                          .append("/default");

DesktopAtoms::HwcCompositionStats::ValidationResult ValidationResultToAtomType(
    ValidationResult result) {
  switch (result) {
    case ValidationResult::kSuccess:
      return DesktopAtoms::HwcCompositionStats::ValidationResult::
          VALIDATION_RESULT_SUCCESS;
    case ValidationResult::kFailure:
      return DesktopAtoms::HwcCompositionStats::ValidationResult::
          VALIDATION_RESULT_FAILURE;
    case ValidationResult::kSkip:
      return DesktopAtoms::HwcCompositionStats::ValidationResult::
          VALIDATION_RESULT_SKIP;
  }
  LOG_ALWAYS_FATAL("Unknown ValidationResult value=%d",
                   static_cast<int>(result));
}

DesktopAtoms::HwcCompositionStats::FlattenReason FlattenReasonToAtomType(
    FlattenReason reason) {
  switch (reason) {
    case FlattenReason::kNone:
      return DesktopAtoms::HwcCompositionStats::FlattenReason::
          FLATTEN_REASON_NONE;
    case FlattenReason::kStaticScene:
      return DesktopAtoms::HwcCompositionStats::FlattenReason::
          FLATTEN_REASON_STATIC_SCENE;
    case FlattenReason::kValidateFailed:
      return DesktopAtoms::HwcCompositionStats::FlattenReason::
          FLATTEN_REASON_VALIDATE_FAILED;
    case FlattenReason::kCtmWithOffset:
      return DesktopAtoms::HwcCompositionStats::FlattenReason::
          FLATTEN_REASON_CTM_WITH_OFFSET;
  }
  LOG_ALWAYS_FATAL("Unknown FlattenReason value=%d", static_cast<int>(reason));
}

std::string ValidationResultToString(ValidationResult result) {
  switch (result) {
    case ValidationResult::kSuccess:
      return "Success";
    case ValidationResult::kFailure:
      return "Failure";
    case ValidationResult::kSkip:
      return "Skip";
  }
  LOG_ALWAYS_FATAL("Unknown ValidationResult value=%d",
                   static_cast<int>(result));
  return "Unknown";
}

std::string FlattenReasonToString(FlattenReason reason) {
  switch (reason) {
    case FlattenReason::kNone:
      return "None";
    case FlattenReason::kStaticScene:
      return "StaticScene";
    case FlattenReason::kValidateFailed:
      return "ValidateFailed";
    case FlattenReason::kCtmWithOffset:
      return "CtmWithOffset";
  }
  LOG_ALWAYS_FATAL("Unknown FlattenReason value=%d", static_cast<int>(reason));
  return "Unknown";
}

// Use a private implementation of CompositionStatsAtomReporter to avoid leaking
// the IStats interface through the public api.
class CompositionStatsReporterDesktop : public CompositionStatsAtomReporter {
 public:
  void PushAtom(int64_t display_handle, bool present_failed,
                ValidationResult validation_result,
                FlattenReason flatten_reason, int64_t frame_count,
                int64_t layer_count, int64_t used_plane_count,
                uint64_t total_pixops, uint64_t gpu_pixops) override {
    ALOGV("Sending stats: display_handle=%" PRId64
          ", present_failed=%d, validation_result=%s, flatten_reason=%s, "
          "frame_count=%" PRId64 ", layer_count=%" PRId64
          ", used_plane_count=%" PRId64 ", total_pixops=%" PRIu64
          ", gpu_pixops=%" PRIu64,
          display_handle, present_failed,
          ValidationResultToString(validation_result).c_str(),
          FlattenReasonToString(flatten_reason).c_str(), frame_count,
          layer_count, used_plane_count, total_pixops, gpu_pixops);

    // The order of the arguments to createVendorAtom is determined by the
    // proto definition in libdesktopatoms.
    const char* kDeprecatedReverseDomainName = "";
    const VendorAtom atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::HWC_COMPOSITION_STATS,
                         kDeprecatedReverseDomainName, display_handle,
                         /*presented_frame_count=*/0,
                         /*present_failed_count=*/0,
                         /*validate_failed_count=*/0, present_failed,
                         ValidationResultToAtomType(validation_result),
                         FlattenReasonToAtomType(flatten_reason), frame_count,
                         layer_count, used_plane_count,
                         static_cast<int64_t>(total_pixops),
                         static_cast<int64_t>(gpu_pixops));

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

std::unique_ptr<CompositionStatsAtomReporter>
CompositionStatsAtomReporter::Create() {
  if (!AServiceManager_isDeclared(kStatsServiceName.c_str())) {
    ALOGW("Stats service is not declared.");
    return nullptr;
  }
  return std::make_unique<CompositionStatsReporterDesktop>();
}

}  // namespace android::drm_hwcomposer
