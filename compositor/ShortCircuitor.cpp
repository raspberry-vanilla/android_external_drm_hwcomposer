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

#include "compositor/ShortCircuitor.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/PresentedCompositionCache.h"
#include "compositor/mapper/MapperUtils.h"
#include "hwc/HwcLayer.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {

[[nodiscard]] bool ContainsCursorLayer(
    const CompositionPlanner::CompositionTypeMap& map, const HwcLayer* layer) {
  const auto itr = map.find(layer);
  return itr != map.end() && itr->second == CompositionType::kCursor;
}

[[nodiscard]] std::optional<size_t> GetCursorLayerIndex(
    const std::vector<CompositionType>& types) {
  return types.back() == CompositionType::kCursor
             ? std::make_optional<size_t>(types.size() - 1)
             : std::nullopt;
}

template <class T>
[[nodiscard]] bool CompareVectors(const std::vector<T>& lhs,
                                  const std::vector<T>& rhs, size_t length) {
  return std::equal(lhs.begin(), lhs.begin() + length, rhs.begin());
}

// If the cursor plane was used for presentation, it has to pass
// heuristic checks again before it can be short circuited.
[[nodiscard]] bool CanContinueUsingCursorPlane(
    const ICompositorDisplay* display, const HwcLayer* topmost_hwclayer,
    const CompositionPlanner::CompositionTypeMap& composition_map) {
  if (ContainsCursorLayer(composition_map, topmost_hwclayer)) {
    return DisplayCanUseCursorPlane(display, topmost_hwclayer);
  }
  return true;  // Cursor plane not previously used. OK to short-circuit.
}

}  // namespace

ShortCircuitor::Config ShortCircuitor::Config::FromProperties() {
  return {.enabled = Properties::ValidationShortCircuiting(),
          .ignore_geometry = Properties::ShortCircuitIgnoreGeometry(),
          .ignore_ctm = Properties::ShortCircuitIgnoreCtm()};
}

bool ShortCircuitor::CheckGeometries(
    const ValidationRequestContext& last_request,
    const ValidationRequestContext& current_request,
    size_t cursor_excluded_length) {
  const auto& last_src_rects = last_request.GetSrcRects();
  const auto& last_display_rects = last_request.GetDisplayRects();

  return CompareVectors(last_src_rects, current_request.GetSrcRects(),
                        cursor_excluded_length) &&
         CompareVectors(last_display_rects, current_request.GetDisplayRects(),
                        cursor_excluded_length);
}

bool ShortCircuitor::Check(
    const Config& config,
    const PresentedCompositionCache::FullContext& presented_ctx,
    const ValidationRequestContext& current_request) {
  using FlattenReason = CompositionPlanner::FlattenReason;

  const auto& [last_request, last_composition] = presented_ctx;
  const auto* last_display = last_request.GetDisplay();
  const auto& last_layers = last_request.GetLayers();
  const auto& last_types = last_request.GetCompositionTypes();

  const auto cursor_idx = GetCursorLayerIndex(last_types);
  const auto cursor_excl_length = cursor_idx.value_or(last_layers.size());

  return
      // Must not be flattened.
      last_composition.flatten_reason == FlattenReason::kNone &&

      // Request context must be recent enough.
      (current_request.GetTimestamp() - last_request.GetTimestamp() <
       config.request_lifetime) &&

      // Must be the same display.
      last_display == current_request.GetDisplay() &&
      // Must have the same color matrix.
      (config.ignore_ctm ||
       current_request.GetColorMatrix() == last_request.GetColorMatrix()) &&

      // Layers must be identical.
      last_layers == current_request.GetLayers() &&
      // Layers must have identical composition types.
      last_types == current_request.GetCompositionTypes() &&

      // Source and destination geometries must be identical (ignoring cursor)
      (config.ignore_geometry ||
       CheckGeometries(last_request, current_request, cursor_excl_length)) &&

      // Alpha values must be identical (ignoring cursor)
      CompareVectors(last_request.GetAlphas(), current_request.GetAlphas(),
                     cursor_excl_length) &&

      // Check if the cursor plane can still be used, if previously being used.
      (!cursor_idx ||
       CanContinueUsingCursorPlane(last_display, last_layers.back(),
                                   last_composition.composition_types));
}

auto ShortCircuitor::Get(const Config& config,
                         const PresentedCompositionCache& last_presentation,
                         const ValidationRequestContext& current_request)
    -> std::optional<CompositionPlanner::ValidatedComposition> {
  if (!config.enabled)
    return std::nullopt;  // Short-circuiting not enabled by sysprop.

  const auto last_presented_ctx = last_presentation.GetContext();
  if (last_presented_ctx &&
      Check(config, *last_presented_ctx, current_request)) {
    return last_presented_ctx->second;
  }
  return std::nullopt;
}

}  // namespace android::drm_hwcomposer
