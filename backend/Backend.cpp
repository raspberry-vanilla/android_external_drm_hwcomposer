/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "Backend.h"

#include <climits>

#include "BackendManager.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "drm/DrmHwc.h"
#include "hwc/HwcDisplay.h"

namespace android::drm_hwcomposer {

namespace {

const HwcLayer* GetCursorLayer(const std::vector<const HwcLayer*>& layers) {
  auto it = std::find_if(layers.begin(), layers.end(),
                         [&](auto* layer) -> bool {
                           return layer->GetSfType() ==
                                  CompositionType::kCursor;
                         });
  if (it == layers.end()) {
    return nullptr;
  }
  return *it;
}

}  // namespace

auto Backend::ValidateDisplay(HwcDisplay* display) -> ValidatedComposition {
  auto layers = display->GetOrderLayersByZPos();

  auto flatcon = display->GetFlatCon();
  if (flatcon) {
    bool should_flatten = false;
    if (layers.size() <= 1)
      flatcon->Disable();
    else
      should_flatten = flatcon->NewFrame();

    if (should_flatten) {
      display->total_stats().frames_flattened++;
      return GetFlattenedComposition(layers);
    }
  }

  bool use_cursor_plane = false;
  const auto* cursor_layer = GetCursorLayer(layers);
  auto cursor_plane = display->GetPipe().GetUsablePlanes().second;
  if (cursor_layer != nullptr && cursor_plane != nullptr &&
      !IsClientLayer(display, cursor_layer) &&
      cursor_plane->Get()->IsValidForLayer(&cursor_layer->GetLayerData())) {
    // Create and test a composition using only cursor plane and all other
    // layers client-composited to infer whether the cursor plane can be used.
    ValidatedComposition cursor_composition{
        .composition_types = GetCompositionTypes(layers, 0, layers.size() - 1,
                                                 /*use_cursor_plane=*/true)};
    use_cursor_plane = display->TestComposition(cursor_composition);
  }

  size_t client_start = 0;
  size_t client_size = 0;
  ValidatedComposition validated_composition;

  // Populates and tests |validated_composition|, returning whether it
  // succeeded.
  auto validate_and_test = [&]() -> bool {
    validated_composition
        .composition_types = GetCompositionTypes(layers, client_start,
                                                 client_size, use_cursor_plane);

    bool testing_needed = client_start != 0 || client_size != layers.size();
    if (testing_needed) {
      return display->TestComposition(validated_composition);
    }

    // Reset the plan in case it was set during a previous test.
    validated_composition.composition_plan = std::make_shared<DrmKmsPlan>();

    return true;
  };

  // Initial composition attempt.
  std::tie(client_start, client_size) = GetClientLayers(display, layers,
                                                        use_cursor_plane);
  bool success = validate_and_test();

  // Cursor fallback: convert all non-cursor layers to client composition and
  // reattempt. (Cursor layer is preserved as _either_ cursor _or_ device
  // composited.)
  if (!success && cursor_layer != nullptr) {
    if (layers.back()->GetSfType() != CompositionType::kCursor) {
      ALOGE("Cursor layer was not found at highest z-order");
      // Continue to next fallback.
    } else {
      client_start = 0;
      client_size = layers.size() - 1;
      success = validate_and_test();
    }
  }

  // Final fallback: convert all layers to client composition.
  if (!success) {
    ++display->total_stats().failed_kms_validate;
    if (use_cursor_plane) {
      ++display->total_stats().failed_kms_cursor_validate;
    }
    use_cursor_plane = false;
    validated_composition = GetFlattenedComposition(layers);
  }

  display->total_stats().gpu_pixops += CalcPixOps(validated_composition);
  display->total_stats().total_pixops += CalcPixOps(layers, 0, layers.size());
  if (use_cursor_plane) {
    ++display->total_stats().cursor_plane_frames;
  }
  return validated_composition;
}

Backend::ValidatedComposition Backend::GetFlattenedComposition(
    const std::vector<const HwcLayer*>& layers) {
  return ValidatedComposition{
      .composition_types = GetCompositionTypes(layers, 0, layers.size(), false),
      .composition_plan = std::make_shared<DrmKmsPlan>()};
}

std::tuple<size_t, size_t> Backend::GetClientLayers(
    const HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
    bool use_cursor_plane) {
  size_t client_start = 0;
  size_t client_size = 0;

  for (size_t z_order = 0; z_order < layers.size(); ++z_order) {
    if (IsClientLayer(display, layers[z_order])) {
      if (client_size == 0) {
        client_start = z_order;
      }
      client_size = (z_order - client_start) + 1;
    }
  }

  return GetExtraClientRange(display, layers, client_start, client_size,
                             use_cursor_plane);
}

bool Backend::IsClientLayer(const HwcDisplay* display, const HwcLayer* layer) {
  return !HardwareSupportsLayerType(layer->GetSfType()) ||
         !layer->IsLayerUsableAsDevice() || display->CtmByGpu() ||
         (layer->GetLayerData().pi.RequireScalingOrPhasing() &&
          display->ForcedScalingWithGpu());
}

bool Backend::HardwareSupportsLayerType(CompositionType comp_type) {
  return comp_type == CompositionType::kDevice ||
         comp_type == CompositionType::kCursor;
}

uint32_t Backend::CalcPixOps(
    const ValidatedComposition& validated_composition) {
  uint32_t pixops = 0;
  for (const auto& [layer, comp_type] :
       validated_composition.composition_types) {
    if (comp_type == CompositionType::kClient) {
      pixops += layer->GetPixOps();
    }
  }
  return pixops;
}

uint32_t Backend::CalcPixOps(const std::vector<const HwcLayer*>& layers,
                             size_t first_z, size_t size) {
  uint32_t pixops = 0;
  ALOGE_IF(first_z + size > layers.size(),
           "CalcPixOps provided range outside of layers");
  for (size_t z_order = first_z;
       z_order < std::min(first_z + size, layers.size()); ++z_order) {
    pixops += layers[z_order]->GetPixOps();
  }
  return pixops;
}

auto Backend::GetCompositionTypes(const std::vector<const HwcLayer*>& layers,
                                  size_t client_first_z, size_t client_size,
                                  bool use_cursor_plane) -> CompositionTypeMap {
  CompositionTypeMap composition_types;
  for (size_t z_order = 0; z_order < layers.size(); ++z_order) {
    if (z_order >= client_first_z && z_order < client_first_z + client_size) {
      composition_types[layers[z_order]] = CompositionType::kClient;
    } else if (use_cursor_plane &&
               layers[z_order]->GetSfType() == CompositionType::kCursor) {
      composition_types[layers[z_order]] = CompositionType::kCursor;
    } else {
      composition_types[layers[z_order]] = CompositionType::kDevice;
    }
  }
  return composition_types;
}

std::tuple<size_t, size_t> Backend::GetExtraClientRange(
    const HwcDisplay* display, const std::vector<const HwcLayer*>& layers,
    size_t client_start, size_t client_size, bool use_cursor_plane) {
  size_t avail_planes = display->GetPipe().GetUsablePlanes().first.size();
  size_t layers_size = layers.size();

  // Cursor plane is not counted among |avail_planes|, so the cursor layer
  // shouldn't be counted in |layers_size|.
  if (use_cursor_plane) {
    ALOGE_IF(layers.empty() ||
                 layers.back()->GetSfType() != CompositionType::kCursor,
             "Cursor layer was not found at highest z-order");
    --layers_size;
  }

  // If there are more layers than planes, save one plane for client composited
  // layers.
  if (avail_planes < layers_size) {
    avail_planes--;
  }

  // If the cursor plane isn't being used, reserve a plane for the cursor to be
  // device composited.
  if (!use_cursor_plane && avail_planes > 0 && layers_size > 0 &&
      layers.back()->GetSfType() == CompositionType::kCursor) {
    avail_planes--;
    layers_size--;
  }

  ALOGE_IF(client_start + client_size > layers.size(),
           "GetExtraClientRange provided client range outside of layers");
  // If extra layers need to be added to the client range, prepare to perform a
  // sliding window search.
  if (layers_size - client_size > avail_planes) {
    const size_t extra_client = (layers_size - client_size) - avail_planes;
    size_t start = 0;
    size_t steps = 0;
    if (client_size != 0) {
      // There are already client layers present, so the window needs to
      // encompass them. Determine the maximum offsets of the ensuing search.
      const size_t prepend = std::min(client_start, extra_client);
      const size_t append = std::min(layers_size - (client_start + client_size), extra_client);
      start = client_start - prepend;
      client_size += extra_client;
      steps = 1 + std::min(std::min(append, prepend), layers_size - (start + client_size));
    } else {
      // There are no other client layers present, so the window may search the
      // entire range.
      client_size = extra_client;
      steps = 1 + layers_size - extra_client;
    }

    // Use a sliding window to determine the client range that results in the
    // fewest GPU pixops.
    uint32_t gpu_pixops = UINT32_MAX;
    for (size_t i = 0; i < steps; i++) {
      const uint32_t po = CalcPixOps(layers, start + i, client_size);
      if (po < gpu_pixops) {
        gpu_pixops = po;
        client_start = start + i;
      }
    }
  }

  return std::make_tuple(client_start, client_size);
}

// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp)
REGISTER_BACKEND("generic", Backend);
// clang-format on

}  // namespace android::drm_hwcomposer
