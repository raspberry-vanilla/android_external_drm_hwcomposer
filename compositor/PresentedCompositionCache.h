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

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/DisplayInfo.h"

namespace android::drm_hwcomposer {

class HwcLayer;
class ICompositorDisplay;
struct DstRectInfo;
struct SrcRectInfo;

// Cached context of the display and the requested layers upon validation
// which can then be queried later by the short-circuiting logic.
class ValidationRequestContext {
 public:
  ValidationRequestContext() = default;
  ValidationRequestContext(const ICompositorDisplay& display,
                           const std::vector<const HwcLayer*>& layers);

  bool Set(const ICompositorDisplay& display,
           const std::vector<const HwcLayer*>& layers);
  void Reset();

  explicit operator bool() const {
    return !layers_.empty();
  }
  const std::vector<const HwcLayer*>& GetLayers() const {
    return layers_;
  }
  const std::vector<CompositionType>& GetCompositionTypes() const {
    return requested_types_;
  }
  const std::vector<SrcRectInfo>& GetSrcRects() const {
    return src_rects_;
  }
  const std::vector<DstRectInfo>& GetDisplayRects() const {
    return display_rects_;
  }
  const std::vector<float>& GetAlphas() const {
    return alphas_;
  }
  const ICompositorDisplay* GetDisplay() const {
    return display_;
  }
  std::shared_ptr<const HalColorTransformMatrix> GetColorMatrix() const {
    return color_matrix_;
  }
  std::chrono::steady_clock::time_point GetTimestamp() const {
    return timestamp_;
  }

 private:
  // Layers and properties.
  std::vector<const HwcLayer*> layers_;  // Pointers may be dangling.
  std::vector<CompositionType> requested_types_;
  std::vector<SrcRectInfo> src_rects_;
  std::vector<DstRectInfo> display_rects_;
  std::vector<float> alphas_;

  // Display properties.
  const ICompositorDisplay* display_ = nullptr;  // Pointer may be dangling.
  std::shared_ptr<const HalColorTransformMatrix> color_matrix_;

  // Timestamp of being set.
  std::chrono::steady_clock::time_point timestamp_;
};

// Cached information on the presented layer composition alongside the
// RequestedCompositionContext associated with it. For short-circuiting.
class PresentedCompositionCache {
 public:
  using RequestedContext = ValidationRequestContext;
  using ValidatedComposition = CompositionPlanner::ValidatedComposition;

  // Always reset the cache regardless of the validity of |requested_context|.
  bool SetRequestedContext(const RequestedContext& requested_context);
  bool SetRequestedContext(RequestedContext&& requested_context);

  // If the requested context is not already set, the whole cache is reset.
  bool SetValidatedComposition(const ValidatedComposition& composition);

  void Reset();

  // Full context consisting of the requested layers, with their properties,
  // upon validation and the validated composition upon presentation.
  using FullContext = std::pair<const RequestedContext&,
                                const ValidatedComposition&>;
  std::optional<FullContext> GetContext() const;

 private:
  RequestedContext requested_context_;

  // composition_->composition_plan is a nullptr.
  std::optional<ValidatedComposition> composition_;
};

}  // namespace android::drm_hwcomposer
