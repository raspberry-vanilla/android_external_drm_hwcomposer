/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <memory>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/LayerData.h"
#include "utils/fd.h"

namespace android {

class HwcDisplay;

class FrontendLayerBase {
 public:
  virtual ~FrontendLayerBase() = default;
};

class HwcLayer {
 public:
  struct Buffer {
    int32_t slot_id;
    std::optional<BufferInfo> bi;
  };
  struct Slot {
    int32_t slot_id;
    SharedFd fence;
  };
  enum class CompositionType {
    kInvalid,
    kClient,
    kDevice,
    kSolidColor,
    kCursor
  };
  // A set of properties to be validated.
  struct LayerProperties {
    std::optional<Buffer> slot_buffer;
    std::optional<Slot> active_slot;
    std::optional<BufferBlendMode> blend_mode;
    std::optional<BufferColorSpace> color_space;
    std::optional<BufferSampleRange> sample_range;
    std::optional<CompositionType> composition_type;
    std::optional<DstRectInfo> display_frame;
    std::optional<float> alpha;
    std::optional<SrcRectInfo> source_crop;
    std::optional<LayerTransform> transform;
    std::optional<uint32_t> z_order;
    std::optional<DamageInfo> damage;
  };

  explicit HwcLayer(HwcDisplay *parent_display) : parent_(parent_display){};

  CompositionType GetSfType() const {
    return sf_type_;
  }
  CompositionType GetValidatedType() const {
    return validated_type_;
  }
  void AcceptTypeChange() {
    sf_type_ = validated_type_;
  }
  void SetValidatedType(CompositionType type) {
    validated_type_ = type;
  }
  bool IsTypeChanged() const {
    return sf_type_ != validated_type_;
  }

  bool GetPriorBufferScanOutFlag() const {
    return prior_buffer_scanout_flag_;
  }

  void SetPriorBufferScanOutFlag(bool state) {
    prior_buffer_scanout_flag_ = state;
  }

  uint32_t GetZOrder() const {
    return z_order_;
  }

  auto &GetLayerData() {
    return layer_data_;
  }

  void SetLayerProperties(const LayerProperties &layer_properties);

  auto GetFrontendPrivateData() -> std::shared_ptr<FrontendLayerBase> {
    return frontend_private_data_;
  }

  auto SetFrontendPrivateData(std::shared_ptr<FrontendLayerBase> data) {
    frontend_private_data_ = std::move(data);
  }

 private:
  // sf_type_ stores the initial type given to us by surfaceflinger,
  // validated_type_ stores the type after running ValidateDisplay
  CompositionType sf_type_ = CompositionType::kInvalid;
  CompositionType validated_type_ = CompositionType::kInvalid;

  uint32_t z_order_ = 0;
  LayerData layer_data_;

  /* The following buffer data can have 2 sources:
   * 1 - Mapper@4 metadata API
   * 2 - HWC@2 API
   * We keep ability to have 2 sources in drm_hwc. It may be useful for CLIENT
   * layer, at this moment HWC@2 API can't specify blending mode for this layer,
   * but Mapper@4 can do that
   */
  BufferColorSpace color_space_{};
  BufferSampleRange sample_range_{};
  BufferBlendMode blend_mode_{};

  bool prior_buffer_scanout_flag_{};

  HwcDisplay *const parent_;

  std::shared_ptr<FrontendLayerBase> frontend_private_data_;

  std::optional<int32_t> active_slot_id_;
  struct BufferSlot {
    BufferInfo bi;
    std::shared_ptr<DrmFbIdHandle> fb;
  };
  std::map<int32_t /*slot*/, BufferSlot> slots_;

  bool ImportFb(BufferSlot &slot) const;

 public:
  void PopulateLayerData();
  void ClearSlots();
  bool IsLayerUsableAsDevice() const;
};

}  // namespace android
