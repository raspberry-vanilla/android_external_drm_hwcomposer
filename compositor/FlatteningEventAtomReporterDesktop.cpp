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

#include "FlatteningEventAtomReporter.h"

#include <cinttypes>
#include <cstdint>
#include <memory>
#include <string>

#include <aidl/android/frameworks/stats/IStats.h>
#include <android/binder_auto_utils.h>
#include <android/binder_manager.h>

#include "compositor/FlatteningController.h"
#include "desktopatoms.h"
#include "utils/log.h"

using aidl::android::frameworks::stats::IStats;
using aidl::android::frameworks::stats::VendorAtom;
namespace DesktopAtoms = android::vendor::google::desktop::stats::DesktopAtoms;

namespace android::drm_hwcomposer {
namespace {

using FlatteningState = FlatteningController::State;

const std::string kStatsServiceName = std::string(IStats::descriptor)
                                          .append("/default");

DesktopAtoms::FlatteningStateChanged::FlatteningState FlatteningStateToAtomType(
    FlatteningState state) {
  switch (state) {
    case FlatteningState::kDisabled:
      return DesktopAtoms::FlatteningStateChanged::FlatteningState::
          FLATTENING_STATE_DISABLED;
    case FlatteningState::kActive:
      return DesktopAtoms::FlatteningStateChanged::FlatteningState::
          FLATTENING_STATE_ACTIVE;
    case FlatteningState ::kFlattened:
      return DesktopAtoms::FlatteningStateChanged::FlatteningState::
          FLATTENING_STATE_FLATTENED;
    case FlatteningState::kTriggeredCallback:
    case FlatteningState::kExitThread:
      return DesktopAtoms::FlatteningStateChanged::FlatteningState::
          FLATTENING_STATE_UNSPECIFIED;
  }
  LOG_ALWAYS_FATAL("Unknown FlatteningController::State value=%d",
                   static_cast<int>(state));
}

std::string StateToString(FlatteningState state) {
  switch (state) {
    case FlatteningState::kDisabled:
      return "Disabled";
    case FlatteningState::kActive:
      return "Active";
    case FlatteningState::kTriggeredCallback:
      return "TriggeredCallback";
    case FlatteningState::kFlattened:
      return "Flattened";
    case FlatteningState::kExitThread:
      return "ExitThread";
  }
  LOG_ALWAYS_FATAL("Unknown FlatteningController::State value=%d",
                   static_cast<int>(state));
}

// Use a private implementation of FlatteningEventAtomReporter to avoid leaking
// the IStats interface through the public api.
class FlatteningEventAtomReporterDesktop : public FlatteningEventAtomReporter {
 public:
  void PushAtom(int64_t display_handle, FlatteningState state) override {
    ALOGV("Sending flattening state change event: display_handle=%" PRId64
          " state=%s",
          display_handle, StateToString(state).c_str());

    const char* kDeprecatedReverseDomainName = "";
    const VendorAtom atom = DesktopAtoms::
        createVendorAtom(DesktopAtoms::FLATTENING_STATE_CHANGED,
                         kDeprecatedReverseDomainName, display_handle,
                         FlatteningStateToAtomType(state));
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

std::unique_ptr<FlatteningEventAtomReporter>
FlatteningEventAtomReporter::Create() {
  if (!AServiceManager_isDeclared(kStatsServiceName.c_str())) {
    ALOGW("Stats service is not declared.");
    return nullptr;
  }
  return std::make_unique<FlatteningEventAtomReporterDesktop>();
}

}  // namespace android::drm_hwcomposer
