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

#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmMode.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

enum class ColorOpType : int32_t {
  /*
   * when a kernel TYPE enum value is not present in color_op_type_enum_map.
   * Pipelines that contain an op with this type are rejected by VerifyColorPipeline()
   * so they are never activated. If it somehow went through to AtomicSetColorPipeline()
   * then op is bypassed.
   */
  kUnknown = -1,
  kMatrix3x4,
  k1DLut,
  k1DLutMultiSegmented,
};

class DrmColorOp : public PipelineBindable<DrmColorOp> {
 public:
  static auto CreateInstance(DrmDevice &dev, uint64_t color_op_id,
                             uint32_t index) -> std::unique_ptr<DrmColorOp>;

  DrmColorOp() = delete;
  DrmColorOp(const DrmColorOp &) = delete;
  DrmColorOp &operator=(const DrmColorOp &) = delete;

  virtual ~DrmColorOp() = default;

  uint32_t GetId() const {
    return id_;
  }

  auto GetIndexInPipeline() const {
    return index_in_pipeline_;
  }

  auto &GetTypeProperty() const {
    return type_;
  }

  auto &GetNextProperty() const {
    return next_;
  }

  auto &GetBypassProperty() const {
    return bypass_;
  }

  auto &GetDataProperty() const {
    return data_;
  }

  auto &GetSizeProperty() const {
    return size_;
  }

  // Convenience method for setting the BYPASS property
  bool SetBypassValue(drmModeAtomicReq &pset, bool bypass) {
    int err = 0;
    uint64_t bypass_value = 0;
    std::tie(err, bypass_value) = bypass ? bypass_.RangeMax()
                                         : bypass_.RangeMin();
    if (err != 0) {
      ALOGE("Failed to get BYPASS range value, errno=%d", err);
      return false;
    }

    return bypass_.AtomicSet(pset, bypass_value);
  };

  std::string DumpState();

 private:
  DrmColorOp(DrmModeColorOpUnique color_op, uint64_t id, uint32_t index)
      : color_op_(std::move(color_op)), id_(id), index_in_pipeline_(index) {};

  DrmModeColorOpUnique color_op_;

  const uint64_t id_;
  const uint32_t index_in_pipeline_;

  DrmProperty type_;
  DrmProperty next_;
  DrmProperty bypass_;
  DrmProperty data_;

  // Optional property
  DrmProperty size_;
};

}  // namespace android::drm_hwcomposer
