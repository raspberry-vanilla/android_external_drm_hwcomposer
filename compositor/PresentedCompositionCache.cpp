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

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/PresentedCompositionCache.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

ValidationRequestContext::ValidationRequestContext(
    const ICompositorDisplay& display,
    const std::vector<const HwcLayer*>& layers) {
  Set(display, layers);
}

// This is the function that guarantees the integrity of all members, i.e.,
// all std::vector's have the same size and the same index corresponds to
// the same layer. However, it doesn't check if |layers| belongs to |display|
// or not.
bool ValidationRequestContext::Set(const ICompositorDisplay& display,
                                   const std::vector<const HwcLayer*>& layers) {
  if (layers.empty())
    return false;

  {
    layers_ = layers;
    const auto layers_size = layers_.size();

    requested_types_.clear();
    requested_types_.reserve(layers_size);
    src_rects_.clear();
    src_rects_.reserve(layers_size);
    display_rects_.clear();
    display_rects_.reserve(layers_size);
    alphas_.clear();
    alphas_.reserve(layers_size);
  }

  for (const auto* layer : layers) {
    requested_types_.emplace_back(layer->GetSfType());

    const auto& layer_data = layer->GetLayerData();
    src_rects_.emplace_back(layer_data.pi.source_crop);
    display_rects_.emplace_back(layer_data.pi.display_frame);
    alphas_.emplace_back(layer_data.pi.alpha);
  }

  display_ = &display;
  color_matrix_ = display.GetColorTransformMatrix();

  timestamp_ = std::chrono::steady_clock::now();

  return true;
}

void ValidationRequestContext::Reset() {
  layers_.clear();
  requested_types_.clear();
  src_rects_.clear();
  display_rects_.clear();
  alphas_.clear();

  display_ = nullptr;
  color_matrix_.reset();
}

bool PresentedCompositionCache::SetRequestedContext(
    const ValidationRequestContext& requested_context) {
  return SetRequestedContext(ValidationRequestContext{requested_context});
}

bool PresentedCompositionCache::SetRequestedContext(
    RequestedContext&& requested_context) {
  composition_.reset();  // Always reset last presented composition.
  requested_context_ = std::move(requested_context);
  return static_cast<bool>(requested_context_);
}

bool PresentedCompositionCache::SetValidatedComposition(
    const ValidatedComposition& composition) {
  if (!requested_context_) {
    Reset();
    return false;
  }

  composition_ = composition;
  composition_->composition_plan.reset();  // Not needed. Remove reference.
  return true;
}

void PresentedCompositionCache::Reset() {
  requested_context_.Reset();
  composition_.reset();
}

std::optional<PresentedCompositionCache::FullContext>
PresentedCompositionCache::GetContext() const {
  if (!requested_context_ || !composition_)
    return std::nullopt;

  return std::make_optional<FullContext>(requested_context_, *composition_);
}

}  // namespace android::drm_hwcomposer
