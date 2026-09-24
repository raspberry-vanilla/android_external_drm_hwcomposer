/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "backend/sdm/sdm_error.h"

#include <string>

#include <core/sdm_types.h>

namespace android::drm_hwcomposer::sdm_error {
std::string ErrorToString(sdm::DisplayError error) {
  switch (error) {
    case sdm::kErrorNone:
      return "sdm::kErrorNone";
    case sdm::kErrorUndefined:
      return "sdm::kErrorUndefined";
    case sdm::kErrorNotSupported:
      return "sdm::kErrorNotSupported";
    case sdm::kErrorPermission:
      return "sdm::kErrorPermission";
    case sdm::kErrorVersion:
      return "sdm::kErrorVersion";
    case sdm::kErrorDataAlignment:
      return "sdm::kErrorDataAlignment";
    case sdm::kErrorInstructionSet:
      return "sdm::kErrorInstructionSet";
    case sdm::kErrorParameters:
      return "sdm::kErrorParameters";
    case sdm::kErrorFileDescriptor:
      return "sdm::kErrorFileDescriptor";
    case sdm::kErrorMemory:
      return "sdm::kErrorMemory";
    case sdm::kErrorResources:
      return "sdm::kErrorResources";
    case sdm::kErrorHardware:
      return "sdm::kErrorHardware";
    case sdm::kErrorTimeOut:
      return "sdm::kErrorTimeOut";
    case sdm::kErrorShutDown:
      return "sdm::kErrorShutDown";
    case sdm::kErrorPerfValidation:
      return "sdm::kErrorPerfValidation";
    case sdm::kErrorNoAppLayers:
      return "sdm::kErrorNoAppLayers";
    case sdm::kErrorRotatorValidation:
      return "sdm::kErrorRotatorValidation";
    case sdm::kErrorNotValidated:
      return "sdm::kErrorNotValidated";
    case sdm::kErrorCriticalResource:
      return "sdm::kErrorCriticalResource";
    case sdm::kErrorDeviceRemoved:
      return "sdm::kErrorDeviceRemoved";
    case sdm::kErrorDriverData:
      return "sdm::kErrorDriverData";
    case sdm::kErrorDeferred:
      return "sdm::kErrorDeferred";
    case sdm::kErrorNeedsCommit:
      return "sdm::kErrorNeedsCommit";
    case sdm::kErrorNeedsValidate:
      return "sdm::kErrorNeedsValidate";
    case sdm::kErrorNeedsLutRegen:
      return "sdm::kErrorNeedsLutRegen";
    case sdm::kErrorNeedsQosRecalc:
      return "sdm::kErrorNeedsQosRecalc";
    case sdm::kErrorNeedsQosRecalcAndLutRegen:
      return "sdm::kErrorNeedsQosRecalcAndLutRegen";
    case sdm::kSeamlessNotAllowed:
      return "sdm::kSeamlessNotAllowed";
    case sdm::kErrorDeviceBusy:
      return "sdm::kErrorDeviceBusy";
    case sdm::kErrorTryAgain:
      return "sdm::kErrorTryAgain";
    case sdm::kErrorConfigMismatch:
      return "sdm::kErrorConfigMismatch";
  }
  return "Unknown error " + std::to_string(error);
}
}  // namespace android::drm_hwcomposer::sdm_error