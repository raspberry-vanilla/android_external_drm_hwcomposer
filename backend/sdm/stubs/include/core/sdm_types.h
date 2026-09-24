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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <utils/fence.h>

namespace sdm {

enum DisplayError {
  kErrorNone = 0,
  kErrorUndefined,
  kErrorNotSupported,
  kErrorPermission,
  kErrorVersion,
  kErrorDataAlignment,
  kErrorInstructionSet,
  kErrorParameters,
  kErrorFileDescriptor,
  kErrorMemory,
  kErrorResources,
  kErrorHardware,
  kErrorTimeOut,
  kErrorShutDown,
  kErrorPerfValidation,
  kErrorNoAppLayers,
  kErrorRotatorValidation,
  kErrorNotValidated,
  kErrorCriticalResource,
  kErrorDeviceRemoved,
  kErrorDriverData,
  kErrorDeferred,
  kErrorNeedsCommit,
  kErrorNeedsValidate,
  kErrorNeedsLutRegen,
  kErrorNeedsQosRecalc,
  kErrorNeedsQosRecalcAndLutRegen,
  kSeamlessNotAllowed,
  kErrorDeviceBusy,
  kErrorTryAgain,
  kErrorConfigMismatch,
};

using LayerId = int64_t;
using nsecs_t = int64_t;

enum {
  SYSTEM_TIME_MONOTONIC = 1,
};

enum class SDMPowerMode {
  POWER_MODE_OFF = 0,
  POWER_MODE_DOZE = 1,
  POWER_MODE_ON = 2,
  POWER_MODE_DOZE_SUSPEND = 3,
};

enum class SDMLayerRequest : int32_t {
  ClearClientTarget = 1,
};

enum SDMTransform : int32_t {
  TRANSFORM_NONE = 0,
  TRANSFORM_FLIP_H = 1,
  TRANSFORM_FLIP_V = 2,
  TRANSFORM_ROT_180 = 3,
  TRANSFORM_ROT_90 = 4,
  TRANSFORM_FLIP_H_ROT_90 = 5,
  TRANSFORM_FLIP_V_ROT_90 = 6,
  TRANSFORM_ROT_270 = 7,
};

struct SDMRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct SDMRegion {
  size_t num_rects = 0;
  std::vector<SDMRect> rects{};
};

struct SDMColor {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 0;
};

struct SDMVsyncPeriodChangeTimeline {};
enum class SDMCameraSmoothOp {};
struct SDMConfigAttributes {};
enum class SDMTUIEventType {};
struct ColorConvertBlitContext {};
struct LayerStitchContext {};

}  // namespace sdm
