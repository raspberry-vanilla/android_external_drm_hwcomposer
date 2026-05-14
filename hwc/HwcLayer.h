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

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

class ICompositorDisplay;

class FrontendLayerBase {
 public:
  virtual ~FrontendLayerBase() = default;
};

class HwcLayer {
 public:
  struct Buffer {
    BufferInfo bi;
    std::shared_ptr<IDrmFbIdHandle> fb;
    SharedFd fence;
  };
  // A set of properties to be validated.
  struct LayerProperties {
    std::optional<Buffer> buffer;
    std::optional<BufferBlendMode> blend_mode;
    std::optional<BufferColorEncoding> color_encoding;
    std::optional<BufferSampleRange> sample_range;
    std::optional<Colorspace> colorspace;
    std::optional<TransferFunction> transfer_func;
    std::optional<CompositionType> composition_type;
    std::optional<DstRectInfo> display_frame;
    std::optional<float> alpha;
    std::optional<SrcRectInfo> source_crop;
    std::optional<LayerTransform> transform;
    std::optional<uint32_t> z_order;
    std::optional<DamageInfo> damage;
    std::optional<float> brightness;
  };

  explicit HwcLayer(ICompositorDisplay *parent_display);
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
    // Occluded layers are exposed as client composited to
    // SurfaceFlinger.
    if (sf_type_ == CompositionType::kClient &&
        validated_type_ == CompositionType::kDeviceOccluded) {
      return false;
    }

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

  const LayerData &GetLayerData() const {
    return layer_data_;
  }

  void SetLayerProperties(const LayerProperties &layer_properties);

  auto GetFrontendPrivateData() -> std::shared_ptr<FrontendLayerBase> {
    return frontend_private_data_;
  }

  auto SetFrontendPrivateData(std::shared_ptr<FrontendLayerBase> data) {
    frontend_private_data_ = std::move(data);
  }

  // Returns the number of pixel operations this layer would require if it were
  // client-composited.
  uint32_t GetPixOps() const;

 private:
  void PopulateLayerData();

  friend class CompositorTestUtils;

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
  BufferColorEncoding color_encoding_{};
  BufferSampleRange sample_range_{};
  BufferBlendMode blend_mode_{};
  Colorspace colorspace_{};
  TransferFunction transfer_func_{};
  float brightness_{};

  bool prior_buffer_scanout_flag_{};

  ICompositorDisplay *const parent_;

  std::shared_ptr<FrontendLayerBase> frontend_private_data_;

  bool has_buffer_set_ = false;

 public:
  void InvalidateBuffer();
  bool IsLayerUsableAsDevice() const;
};

}  // namespace android::drm_hwcomposer
