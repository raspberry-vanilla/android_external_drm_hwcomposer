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

#include "DrmColorOp.h"

#include <xf86drmMode.h>

#include <cinttypes>
#include <cstdint>
#include <sstream>

#include "DrmDevice.h"
#include "drm_mode.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

static int GetColorOpProperty(const DrmDevice &dev, const DrmColorOp &color_op,
                              const char *prop_name, DrmProperty *property) {
  return dev.GetProperty(color_op.GetId(), DRM_MODE_OBJECT_COLOROP, prop_name,
                         property);
}

std::string DrmColorOp::DumpState() {
  std::stringstream ss;
  ss << "Color Operation " << id_ << "\n";
  ss << "├─ \"" << type_.GetName() << "\" = "
     << type_.GetEnumNameFromValue(type_.GetValue().value_or(0)).value_or("")
     << "\n";
  ss << "└─ \"" << next_.GetName() << "\" = " << next_.GetValue().value_or(-1);
  return ss.str();
}

auto DrmColorOp::CreateInstance(DrmDevice &dev, uint64_t color_op_id,
                                uint32_t index) -> std::unique_ptr<DrmColorOp> {
  auto color_op = MakeDrmModeColorOpUnique(*dev.GetFd(), color_op_id);
  if (!color_op) {
    ALOGE("Failed to get ColorOp %" PRIu64 ", index %d", color_op_id, index);
    return {};
  }

  auto c = std::unique_ptr<DrmColorOp>(
      new DrmColorOp(std::move(color_op), color_op_id, index));

  if (GetColorOpProperty(dev, *c, "TYPE", &c->type_) != 0 ||
      !c->type_.GetValue().has_value()) {
    ALOGE("Failed to get TYPE property");
    return {};
  }

  if (GetColorOpProperty(dev, *c, "NEXT", &c->next_) != 0 ||
      !c->next_.GetValue().has_value()) {
    ALOGE("Failed to get NEXT property");
    return {};
  }

  if (GetColorOpProperty(dev, *c, "BYPASS", &c->bypass_) != 0) {
    ALOGE("Failed to get BYPASS property");
    return {};
  }

  if (GetColorOpProperty(dev, *c, "DATA", &c->data_) != 0) {
    ALOGE("Failed to get DATA property");
    return {};
  }

  return c;
}

}  // namespace android::drm_hwcomposer
