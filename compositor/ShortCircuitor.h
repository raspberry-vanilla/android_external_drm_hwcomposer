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
#include <cstddef>
#include <optional>

#include "compositor/CompositionPlanner.h"
#include "compositor/PresentedCompositionCache.h"

namespace android::drm_hwcomposer {

enum class CompositionType;
class ICompositorDisplay;
class HwcLayer;

class ShortCircuitor {
 public:
  constexpr static auto kDefaultRequestCtxLifetime = std::chrono::milliseconds(
      100);

  struct Config {
    bool enabled = false;
    bool ignore_geometry = true;
    bool ignore_ctm = true;
    std::chrono::milliseconds request_lifetime = kDefaultRequestCtxLifetime;

    [[nodiscard]] static Config FromProperties();
  };

  // Assumes that all pointers in |current_request| are valid.
  [[nodiscard]] static auto Get(
      const Config& config, const PresentedCompositionCache& last_presentation,
      const ValidationRequestContext& current_request)
      -> std::optional<CompositionPlanner::ValidatedComposition>;

 private:
  [[nodiscard]] static bool CheckGeometries(
      const ValidationRequestContext& last_request,
      const ValidationRequestContext& current_request,
      size_t cursor_excluded_length);

  [[nodiscard]] static bool Check(
      const Config& config,
      const PresentedCompositionCache::FullContext& presented_ctx,
      const ValidationRequestContext& current_request);
};

}  // namespace android::drm_hwcomposer
