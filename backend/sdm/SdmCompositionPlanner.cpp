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

#define LOG_TAG "drmhwc"  // NOLINT(cppcoreguidelines-macro-usage)

#include "backend/sdm/SdmCompositionPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <system/graphics-base-v1.0.h>

#include <core/buffer_allocator.h>  // IWYU pragma: keep
#include <core/layer_stack.h>
#include <core/sdm_types.h>
#include <sdm_display_intf_drawcycle.h>
#include <sdm_display_intf_layer_builder.h>
#include <utils/fence.h>

#include "backend/sdm/SnapAllocHandle.h"
#include "backend/sdm/sdm_error.h"
#include "bufferinfo/BufferInfo.h"
#include "compositor/CompositionPlanner.h"
#include "compositor/DisplayInfo.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "hwc/HwcLayer.h"
#include "utils/fd.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {
namespace {
using sdm_error::ErrorToString;

// SDMDisplayLayerBuilderIntf::SetLayerCompositionType takes an int32_t but the
// acceptable values are not defined or documented. QTI AidlComposerClient shows
// that the value passed in from HWC3 is sent through directly. Redefine the
// supported HWC3 AIDL values here.
enum class SDMLayerComposition : int32_t {
  kInvalid = 0,
  kClient = 1,
  kDevice = 2,
  kSolidColor = 3,
  kCursor = 4
};

CompositionType SdmCompositionTypeToDrmHwc(int32_t sdm_composition_type) {
  switch (sdm_composition_type) {
    case static_cast<int32_t>(SDMLayerComposition::kInvalid):
      return CompositionType::kInvalid;
    case static_cast<int32_t>(SDMLayerComposition::kClient):
      return CompositionType::kClient;
    case static_cast<int32_t>(SDMLayerComposition::kDevice):
      return CompositionType::kDevice;
    case static_cast<int32_t>(SDMLayerComposition::kSolidColor):
      return CompositionType::kSolidColor;
    case static_cast<int32_t>(SDMLayerComposition::kCursor):
      return CompositionType::kCursor;
    default:
      break;
  }
  ALOGE("Invalid composition type: %d", sdm_composition_type);
  return CompositionType::kInvalid;
}

android_dataspace_t ReconstructDataspace(HwcColorspace colorspace,
                                         TransferFunction tf,
                                         BufferSampleRange range) {
  // HAL_DATASPACE_UNKNOWN maps to HAL_DATASPACE_STANDARD_UNSPECIFIED,
  // HAL_DATASPACE_TRANSFER_UNSPECIFIED and HAL_DATASPACE_RANGE_UNSPECIFIED
  // respectively. When received, HWC should have some appropriate default
  // behavior in this case. These values are not indicative of an error.
  int32_t standard = HAL_DATASPACE_STANDARD_UNSPECIFIED;
  switch (colorspace) {
    case HwcColorspace::kBt709:
      standard = HAL_DATASPACE_STANDARD_BT709;
      break;
    case HwcColorspace::kBt601:
      standard = HAL_DATASPACE_STANDARD_BT601_625;
      break;
    case HwcColorspace::kDciP3:
      standard = HAL_DATASPACE_STANDARD_DCI_P3;
      break;
    case HwcColorspace::kBt2020:
      standard = HAL_DATASPACE_STANDARD_BT2020;
      break;
    case HwcColorspace::kDefault:
      break;
  }

  int32_t transfer = HAL_DATASPACE_TRANSFER_UNSPECIFIED;
  switch (tf) {
    case TransferFunction::kPq:
      transfer = HAL_DATASPACE_TRANSFER_ST2084;
      break;
    case TransferFunction::kHlg:
      transfer = HAL_DATASPACE_TRANSFER_HLG;
      break;
    case TransferFunction::kSrgb:
      transfer = HAL_DATASPACE_TRANSFER_SRGB;
      break;
    case TransferFunction::kSmpte170M:
      transfer = HAL_DATASPACE_TRANSFER_SMPTE_170M;
      break;
    case TransferFunction::kUnknown:
      break;
  }

  int32_t range_val = HAL_DATASPACE_RANGE_UNSPECIFIED;
  switch (range) {
    case BufferSampleRange::kFullRange:
      range_val = HAL_DATASPACE_RANGE_FULL;
      break;
    case BufferSampleRange::kLimitedRange:
      range_val = HAL_DATASPACE_RANGE_LIMITED;
      break;
    case BufferSampleRange::kUndefined:
      break;
  }

  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<android_dataspace_t>(standard | transfer | range_val);
}

bool ForceClientComposition(const HwcLayer* layer) {
  // b/557260536: Solid color layers are not working properly
  // in SDM when serviced by device. Force all solid color
  // layers to client.
  // TODO(b/557961309): Re-visit this once SDM is fixed.
  return layer->GetSfType() == CompositionType::kSolidColor;
}

}  // namespace

SdmCompositionPlanner::SdmCompositionPlanner(
    uint64_t sdm_display_id, sdm::SDMDisplayLayerBuilderIntf* layer_intf,
    sdm::SDMDisplayDrawCycleIntf* draw_cycle_intf)
    : sdm_display_id_(sdm_display_id),
      layer_intf_(layer_intf),
      draw_cycle_intf_(draw_cycle_intf) {
}

SdmCompositionPlanner::~SdmCompositionPlanner() {
  if (!layer_mappings_.empty()) {
    draw_cycle_intf_->WaitForDrawCycleToComplete(sdm_display_id_);
    for (const auto& [_, sdm_layer_id] : layer_mappings_) {
      auto display_error = layer_intf_->DestroyLayer(sdm_display_id_,
                                                     sdm_layer_id);
      ALOGE_IF(display_error != sdm::kErrorNone,
               "DestroyLayer failed during deconstruction: %s",
               ErrorToString(display_error).c_str());
    }
  }
}

auto SdmCompositionPlanner::ValidateDisplay(const ICompositorDisplay* display)
    -> ValidationResult {
  // Create/destroy new/old layers and update the layer properties for each
  // layer.
  UpdateLayerMapping(*display);

  // Plumb each layer's properties into the corresponding SDM layers. Initialize
  // the composition_type_map with the composition type initially requested from
  // the client.
  CompositionPlanner::CompositionTypeMap composition_type_map;
  for (const auto* layer : display->GetOrderLayersByZPos()) {
    CompositionType composition_type = layer->GetSfType();
    if (ForceClientComposition(layer)) {
      composition_type = CompositionType::kClient;
    }
    UpdateLayer(layer, composition_type);
    composition_type_map[layer] = composition_type;
  }

  // Notify SDM that layers have been updated, so internal bookkeeping can be
  // updated.
  draw_cycle_intf_->LayerStackUpdated(sdm_display_id_);

  // TODO: validate_only is hardcoded right now. Handle validateOrPresent case.
  const bool validate_only = true;

  std::shared_ptr<sdm::Fence> out_retire_fence;
  // num_types and num_requests are not used.
  uint32_t out_num_types = 0;
  uint32_t out_num_requests = 0;

  // This will always be true because validate_only is true.
  bool out_needs_commit = false;
  auto display_error = draw_cycle_intf_->CommitOrPrepare(sdm_display_id_,
                                                         validate_only,
                                                         &out_retire_fence,
                                                         &out_num_types,
                                                         &out_num_requests,
                                                         &out_needs_commit);
  ALOGE_IF(display_error != sdm::kErrorNone &&
               display_error != sdm::kErrorNeedsCommit,
           "CommitOrPrepare failed: %s", ErrorToString(display_error).c_str());

  display_error = draw_cycle_intf_->AcceptDisplayChanges(sdm_display_id_);
  ALOGE_IF(display_error != sdm::kErrorNone, "AcceptDisplayChanges failed: %s",
           ErrorToString(display_error).c_str());

  // Read back the changed composition types from SDM.
  // Initialize the size of the vectors to the number of layers, which is the
  // max possible number of changes.
  uint32_t num_elements = 0;
  std::vector<SDMLayerId> changed_layers(layer_mappings_.size());
  std::vector<int32_t> changed_composition_types(layer_mappings_.size());
  display_error = draw_cycle_intf_
                      ->GetChangedCompositionTypes(sdm_display_id_,
                                                   &num_elements,
                                                   &changed_layers.front(),
                                                   &changed_composition_types
                                                        .front());
  ALOGE_IF(display_error != sdm::kErrorNone,
           "GetChangedCompositionTypes failed: %s",
           ErrorToString(display_error).c_str());

  if (num_elements > layer_mappings_.size()) {
    ALOGE(
        "GetChangedCompositionTypes out_num_elements is larger than the "
        "number of layers.");
    num_elements = layer_mappings_.size();
  }
  // Iterate through the changed_layers and update the CompositionTypeMap with
  // the corresponding changed_composition_type.
  for (uint32_t i = 0; i < num_elements; i++) {
    const auto* hwc_layer = GetHwcLayer(changed_layers[i]);
    if (hwc_layer == nullptr) {
      ALOGE("couldn't find hwc layer id for sdm layer id: %d",
            static_cast<int>(changed_layers[i]));
      continue;
    }
    if (composition_type_map.count(hwc_layer) == 0) {
      ALOGE("HwcLayer was not in the CompositionTypeMap.");
      continue;
    }
    composition_type_map[hwc_layer] = SdmCompositionTypeToDrmHwc(
        changed_composition_types[i]);
  }

  // Get DisplayRequests from SDM.
  int32_t display_reqs = 0;
  std::vector<sdm::LayerId> requested_layers(layer_mappings_.size());
  std::vector<int32_t> requested_masks(layer_mappings_.size());
  display_error = draw_cycle_intf_
                      ->GetDisplayRequests(sdm_display_id_, &display_reqs,
                                           &num_elements,
                                           &requested_layers.front(),
                                           &requested_masks.front());
  ALOGE_IF(display_error != sdm::kErrorNone, "GetDisplayRequests failed: %s",
           ErrorToString(display_error).c_str());

  if (num_elements > requested_layers.size()) {
    ALOGE(
        "GetDisplayRequests out_num_elements is larger than the "
        "number of layers.");
    num_elements = requested_layers.size();
  }
  // This doesn't appear to be set in SDM.
  ALOGW_IF(display_reqs != 0, "Got unexpected display_reqs: %d", display_reqs);

  // For each SDM layer id, find the corresponding HwcLayer.
  std::vector<const HwcLayer*> punch_out_layers;
  for (uint32_t i = 0; i < num_elements; i++) {
    const auto* hwc_layer = GetHwcLayer(requested_layers[i]);
    if (hwc_layer == nullptr) {
      ALOGE("couldn't find hwc layer id for sdm layer id: %d",
            static_cast<int>(requested_layers[i]));
      continue;
    }
    int32_t mask = requested_masks[i];
    if ((mask &
         static_cast<int32_t>(sdm::SDMLayerRequest::ClearClientTarget)) != 0) {
      punch_out_layers.push_back(hwc_layer);
    }
  }

  // LayerToPlaneJoiningPlan.plane is always set to nullptr. This is fine
  // because drm resource tracking is done in the SDM library, and
  // drm_hwcomposer only uses the plane in the drm-based composition planner and
  // commit sink. If we need to make drm commits affecting these planes in
  // shared drm_hwcomposer code, then we will need to figure out how to track
  // the drm resources here.
  auto composition_plan = std::make_shared<LayerToPlaneJoiningPlan>();
  composition_plan->plan.reserve(composition_type_map.size() + 1);
  bool has_client = false;
  for (const auto [layer, composition_type] : composition_type_map) {
    switch (composition_type) {
      // Represent Device and Cursor composition in the plan.
      case CompositionType::kDevice:
      case CompositionType::kCursor:
        composition_plan->plan.emplace_back(LayerData(layer->GetLayerData()),
                                            nullptr,
                                            static_cast<int>(
                                                layer->GetZOrder()));
        break;
        // Client layer will be appended to the plan.
      case CompositionType::kClient:
        has_client = true;
        break;
      case CompositionType::kSolidColor:
        // Solid color is not represented in the plan.
        break;
      case CompositionType::kInvalid:
        ALOGE("Invalid composition type.");
        break;
      case CompositionType::kDeviceOccluded:
        ALOGE("LayerCaching not yet supported, but got kDeviceOccluded.");
        break;
    }
  }
  // Add another LayertoPlaneJoiningPlan representing the client target buffer.
  if (has_client) {
    const HwcLayer* layer = &display->GetClientLayer();
    composition_plan->plan.emplace_back(LayerData(layer->GetLayerData()),
                                        nullptr,
                                        static_cast<int>(layer->GetZOrder()));
    composition_plan->client_z_order = composition_plan->plan.size() - 1;
  }

  // TODO: Plumb the validation and flattening stats.
  return {.composition =
              ValidatedComposition{
                  .composition_types = composition_type_map,
                  .punch_out_layers = punch_out_layers,
                  .composition_plan = composition_plan,
                  .flatten_reason = FlattenReason::kNone,
                  .cursor_plane_validated = std::nullopt,
              },
          .short_circuited = false};
}

void SdmCompositionPlanner::UpdateLayerMapping(
    const ICompositorDisplay& display) {
  const std::vector<const HwcLayer*> hwc_layers = display
                                                      .GetOrderLayersByZPos();

  // Create new SDM layers if needed.
  for (const auto* layer : hwc_layers) {
    if (layer_mappings_.count(layer) > 0) {
      continue;
    }
    SDMLayerId new_sdm_layer_id = 0;
    auto display_error = layer_intf_->CreateLayer(sdm_display_id_,
                                                  &new_sdm_layer_id);
    if (display_error != sdm::kErrorNone) {
      ALOGE("CreateLayer failed: %s", ErrorToString(display_error).c_str());
      continue;
    }
    layer_mappings_[layer] = new_sdm_layer_id;
  }

  // New layers have been added, and stale layers have not yet been destroyed.
  // If the sizes of these structures are not the same, then there are some
  // stale layers that need to be deleted.
  if (layer_mappings_.size() > hwc_layers.size()) {
    // This needs to be called to ensure async commit job is complete before
    // destroying layers.
    draw_cycle_intf_->WaitForDrawCycleToComplete(sdm_display_id_);
    // Iterate through the HwcLayer->SdmLayer mapping and delete SDM layers if
    // needed.
    for (auto it = layer_mappings_.begin(); it != layer_mappings_.end();) {
      if (std::find(hwc_layers.begin(), hwc_layers.end(), it->first) !=
          hwc_layers.end()) {
        ++it;
        continue;
      }

      auto display_error = layer_intf_->DestroyLayer(sdm_display_id_,
                                                     it->second);
      ALOGE_IF(display_error != sdm::kErrorNone, "DestroyLayer failed: %s",
               ErrorToString(display_error).c_str());
      it = layer_mappings_.erase(it);
    }
  }
}

void SdmCompositionPlanner::UpdateLayer(const HwcLayer* hwc_layer,
                                        CompositionType composition_type) {
  if (layer_mappings_.count(hwc_layer) == 0) {
    ALOGE("couldn't find sdm layer for hwc layer");
    return;
  }
  int64_t sdm_layer_id = layer_mappings_[hwc_layer];
  const LayerData& layer_data = hwc_layer->GetLayerData();

  // Update all relevant layer properties.
  UpdateLayerBuffer(sdm_layer_id, layer_data);
  UpdateLayerBlendMode(sdm_layer_id, layer_data);
  UpdateLayerDisplayFrame(sdm_layer_id, layer_data);
  UpdateLayerSolidColor(sdm_layer_id, layer_data);
  UpdateLayerAlpha(sdm_layer_id, layer_data);
  UpdateLayerSourceCrop(sdm_layer_id, layer_data);
  UpdateLayerTransform(sdm_layer_id, layer_data);
  UpdateZOrder(sdm_layer_id, hwc_layer->GetZOrder());
  UpdateCompositionType(sdm_layer_id, composition_type);
  UpdateLayerDataspace(sdm_layer_id, layer_data);
  UpdateLayerBrightness(sdm_layer_id, layer_data);

  // TODO: damage and visible region, and others.
}

void SdmCompositionPlanner::UpdateLayerBuffer(SDMLayerId sdm_layer_id,
                                              const LayerData& layer_data) {
  if (!layer_data.bi.has_value()) {
    return;
  }
  if (layer_data.bi->fds_shared == nullptr) {
    ALOGE("layer_data.fds_shared is null");
    return;
  }
  std::shared_ptr<SnapAllocHandle>
      snap_handle = std::static_pointer_cast<SnapAllocHandle>(
          layer_data.bi->fds_shared);

  std::shared_ptr<sdm::Fence> acquire_fence;
  if (layer_data.acquire_fence != nullptr) {
    acquire_fence = sdm::Fence::Create(DupFd(layer_data.acquire_fence),
                                       "acquire_fence");
  }
  auto display_error = layer_intf_->SetLayerBuffer(sdm_display_id_,
                                                   sdm_layer_id,
                                                   snap_handle->GetSnapHandle(),
                                                   acquire_fence);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerBuffer failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerBlendMode(SDMLayerId sdm_layer_id,
                                                 const LayerData& layer_data) {
  if (!layer_data.bi) {
    return;
  }
  sdm::LayerBlending sdm_blend_mode = sdm::kBlendingSkip;
  switch (layer_data.bi->blend_mode) {
    case BufferBlendMode::kNone:
      sdm_blend_mode = sdm::kBlendingOpaque;
      break;
    case BufferBlendMode::kPreMult:
      sdm_blend_mode = sdm::kBlendingPremultiplied;
      break;
    case BufferBlendMode::kCoverage:
      sdm_blend_mode = sdm::kBlendingCoverage;
      break;
    case BufferBlendMode::kUndefined:
      ALOGE("BufferBlendMode::kUndefined for layer.");
      return;
  }
  auto display_error = layer_intf_->SetLayerBlendMode(sdm_display_id_,
                                                      sdm_layer_id,
                                                      sdm_blend_mode);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerBlendMode failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerDisplayFrame(
    SDMLayerId sdm_layer_id, const LayerData& layer_data) {
  if (!layer_data.pi.display_frame.i_rect.has_value()) {
    return;
  }
  sdm::SDMRect frame{.left = layer_data.pi.display_frame.i_rect->left,
                     .top = layer_data.pi.display_frame.i_rect->top,
                     .right = layer_data.pi.display_frame.i_rect->right,
                     .bottom = layer_data.pi.display_frame.i_rect->bottom};
  auto display_error = layer_intf_->SetLayerDisplayFrame(sdm_display_id_,
                                                         sdm_layer_id, frame);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerDisplayFrame failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerSolidColor(SDMLayerId sdm_layer_id,
                                                  const LayerData& layer_data) {
  if (!layer_data.solid_color.has_value()) {
    return;
  }

  // Transaction#SetColor in SurfaceControl.java mandates channel value of [0,
  // 1f]. RenderEngine also receives unmodifed channel value. Should really just
  // be float and be left to SDM to convert considering the dataspace.
  constexpr float kMaxColorChannelValue = 255.0F;
  sdm::SDMColor color{
      .r = static_cast<uint8_t>(
          std::round(layer_data.solid_color->r * kMaxColorChannelValue)),
      .g = static_cast<uint8_t>(
          std::round(layer_data.solid_color->g * kMaxColorChannelValue)),
      .b = static_cast<uint8_t>(
          std::round(layer_data.solid_color->b * kMaxColorChannelValue)),
      // OutputLayer::writeSolidColorStateToHWC() in SF hard codes the alpha
      // channel to 1.0F.
      .a = static_cast<uint8_t>(
          std::round(layer_data.solid_color->a * kMaxColorChannelValue)),
  };

  auto display_error = layer_intf_->SetLayerColor(sdm_display_id_, sdm_layer_id,
                                                  color);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerColor failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerAlpha(SDMLayerId sdm_layer_id,
                                             const LayerData& layer_data) {
  auto display_error = layer_intf_->SetLayerPlaneAlpha(sdm_display_id_,
                                                       sdm_layer_id,
                                                       layer_data.pi.alpha);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerPlaneAlpha failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerSourceCrop(SDMLayerId sdm_layer_id,
                                                  const LayerData& layer_data) {
  if (!layer_data.pi.source_crop.f_rect.has_value()) {
    return;
  }

  // Use ceil/floor to get an int rect per the documentation for sourceCrop:
  // https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/graphics/composer/aidl/android/hardware/graphics/composer3/LayerCommand.aidl
  sdm::SDMRect crop{.left = (int)ceilf(layer_data.pi.source_crop.f_rect->left),
                    .top = (int)ceilf(layer_data.pi.source_crop.f_rect->top),
                    .right = (int)floorf(
                        layer_data.pi.source_crop.f_rect->right),
                    .bottom = (int)floorf(
                        layer_data.pi.source_crop.f_rect->bottom)};
  auto display_error = layer_intf_->SetLayerSourceCrop(sdm_display_id_,
                                                       sdm_layer_id, crop);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerSourceCrop failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerTransform(SDMLayerId sdm_layer_id,
                                                 const LayerData& layer_data) {
  int32_t sdm_transform = sdm::SDMTransform::TRANSFORM_NONE;
  if (layer_data.pi.transform.hflip) {
    sdm_transform |= sdm::SDMTransform::TRANSFORM_FLIP_H;
  }
  if (layer_data.pi.transform.vflip) {
    sdm_transform |= sdm::SDMTransform::TRANSFORM_FLIP_V;
  }
  if (layer_data.pi.transform.rotate90) {
    sdm_transform |= sdm::SDMTransform::TRANSFORM_ROT_90;
  }
  auto display_error = layer_intf_
                           ->SetLayerTransform(sdm_display_id_, sdm_layer_id,
                                               static_cast<sdm::SDMTransform>(
                                                   sdm_transform));

  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerTransform failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateZOrder(SDMLayerId sdm_layer_id,
                                         uint32_t z_order) {
  auto display_error = layer_intf_->SetLayerZOrder(sdm_display_id_,
                                                   sdm_layer_id, z_order);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerZOrder failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateCompositionType(
    SDMLayerId sdm_layer_id, CompositionType composition_type) {
  // Translate to sdm composition type.
  SDMLayerComposition sdm_composition_type = SDMLayerComposition::kClient;
  switch (composition_type) {
    case CompositionType::kClient:
      sdm_composition_type = SDMLayerComposition::kClient;
      break;
    case CompositionType::kDevice:
    case CompositionType::kDeviceOccluded:
      sdm_composition_type = SDMLayerComposition::kDevice;
      break;
    case CompositionType::kSolidColor:
      sdm_composition_type = SDMLayerComposition::kSolidColor;
      break;
    case CompositionType::kCursor:
      sdm_composition_type = SDMLayerComposition::kCursor;
      break;
    case CompositionType::kInvalid:
      sdm_composition_type = SDMLayerComposition::kInvalid;
      break;
  }
  auto display_error = layer_intf_
                           ->SetLayerCompositionType(sdm_display_id_,
                                                     sdm_layer_id,
                                                     static_cast<int32_t>(
                                                         sdm_composition_type));
  ALOGE_IF(display_error != sdm::kErrorNone,
           "SetLayerCompositionType failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerDataspace(SDMLayerId sdm_layer_id,
                                                 const LayerData& layer_data) {
  BufferSampleRange range = BufferSampleRange::kUndefined;
  if (layer_data.bi.has_value()) {
    range = layer_data.bi->sample_range;
  }
  android_dataspace_t dataspace = ReconstructDataspace(layer_data.colorspace,
                                                       layer_data.transfer_func,
                                                       range);
  auto display_error = layer_intf_->SetLayerDataspace(sdm_display_id_,
                                                      sdm_layer_id,
                                                      static_cast<int32_t>(
                                                          dataspace));
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerDataspace failed: %s",
           ErrorToString(display_error).c_str());
}

void SdmCompositionPlanner::UpdateLayerBrightness(SDMLayerId sdm_layer_id,
                                                  const LayerData& layer_data) {
  if (!layer_data.brightness.has_value()) {
    return;
  }
  auto display_error = layer_intf_->SetLayerBrightness(sdm_display_id_,
                                                       sdm_layer_id,
                                                       *layer_data.brightness);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetLayerBrightness failed: %s",
           ErrorToString(display_error).c_str());
}

const HwcLayer* SdmCompositionPlanner::GetHwcLayer(SDMLayerId layer_id) const {
  for (const auto& [hwc_layer, sdm_layer_id] : layer_mappings_) {
    if (sdm_layer_id == layer_id) {
      return hwc_layer;
    }
  }
  return nullptr;
}

}  // namespace android::drm_hwcomposer
