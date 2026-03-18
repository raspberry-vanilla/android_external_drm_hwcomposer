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

#include "HwcLayer.h"

#include "compositor/ICompositorDisplay.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

HwcLayer::HwcLayer(ICompositorDisplay* parent_display)
    : parent_(parent_display) {
}

void HwcLayer::SetLayerProperties(const LayerProperties& layer_properties) {
  if (layer_properties.buffer) {
    has_buffer_set_ = true;
    layer_data_.bi = layer_properties.buffer->bi;
    layer_data_.fb = layer_properties.buffer->fb;
    layer_data_.acquire_fence = layer_properties.buffer->fence;
  }
  if (layer_properties.blend_mode) {
    blend_mode_ = layer_properties.blend_mode.value();
  }
  if (layer_properties.colorspace) {
    colorspace_ = layer_properties.colorspace.value();
  }
  if (layer_properties.color_encoding) {
    color_encoding_ = layer_properties.color_encoding.value();
  }
  if (layer_properties.sample_range) {
    sample_range_ = layer_properties.sample_range.value();
  }
  if (layer_properties.transfer_func) {
    transfer_func_ = layer_properties.transfer_func.value();
  }
  if (layer_properties.composition_type) {
    sf_type_ = layer_properties.composition_type.value();
  }
  if (layer_properties.display_frame) {
    layer_data_.pi.display_frame = layer_properties.display_frame.value();
  }
  if (layer_properties.alpha) {
    layer_data_.pi.alpha = layer_properties.alpha.value();
  }
  if (layer_properties.source_crop) {
    layer_data_.pi.source_crop = layer_properties.source_crop.value();
  }
  if (layer_properties.transform) {
    layer_data_.pi.transform = layer_properties.transform.value();
  }
  if (layer_properties.z_order) {
    z_order_ = layer_properties.z_order.value();
  }
  if (layer_properties.damage) {
    layer_data_.pi.damage = layer_properties.damage.value();
  }

  if (has_buffer_set_) {
    PopulateLayerData();
  }
}

void HwcLayer::PopulateLayerData() {
  if (!layer_data_.bi) {
    ALOGE("Internal error: PopulateLayerData called without valid bi.");
    return;
  }

  if (blend_mode_ != BufferBlendMode::kUndefined) {
    layer_data_.bi->blend_mode = blend_mode_;
  }
  if (colorspace_ != Colorspace::kDefault) {
    layer_data_.colorspace = colorspace_;
  }
  if (color_encoding_ != BufferColorEncoding::kUndefined) {
    layer_data_.bi->color_encoding = color_encoding_;
  }
  if (sample_range_ != BufferSampleRange::kUndefined) {
    layer_data_.bi->sample_range = sample_range_;
  }
  if (transfer_func_ != TransferFunction::kUnknown) {
    layer_data_.transfer_func = transfer_func_;
  }
}

void HwcLayer::InvalidateBuffer() {
  has_buffer_set_ = false;
}

/* Check that the layer has an active slot set, and there is a valid
   * framebuffer in the active slot.
 */
bool HwcLayer::IsLayerUsableAsDevice() const {
  if (!has_buffer_set_) {
    return false;
  }
  return layer_data_.fb != nullptr;
}

uint32_t HwcLayer::GetPixOps() const {
  const auto& df = GetLayerData().pi.display_frame;
  if (df.i_rect.has_value()) {
    return df.i_rect->Width() * df.i_rect->Height();
  }
  return parent_->GetSize().first * parent_->GetSize().second;
}

}  // namespace android::drm_hwcomposer
