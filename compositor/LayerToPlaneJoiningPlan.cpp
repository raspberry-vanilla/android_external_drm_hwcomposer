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

#include "LayerToPlaneJoiningPlan.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/LayerData.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmPlane.h"

namespace android::drm_hwcomposer {

auto LayerToPlaneJoiningPlan::CreateLayerToPlaneJoiningPlan(
    const DrmDisplayPipeline &pipe, std::vector<LayerData> composition,
    std::optional<LayerData> cursor_layer)
    -> std::unique_ptr<LayerToPlaneJoiningPlan> {
  auto [avail_planes, cursor_plane] = pipe.GetUsablePlanes();

  if (cursor_layer) {
    if (!cursor_plane ||
        !cursor_plane->Get()->IsValidForLayer(&cursor_layer.value())) {
      // Cursor plane can't be used. The cursor layer may need to fallback to
      // device or client composition.
      return {};
    }
  }

  auto plan = std::make_unique<LayerToPlaneJoiningPlan>();
  plan->plan.reserve(composition.size() +
                     static_cast<size_t>(cursor_layer.has_value()));

  auto first_avail_plane = avail_planes.begin();
  for (auto &dhl : composition) {
    // Consume avail_planes until a valid plane is found for the layer.

    const auto suitable_plane = std::find_if(first_avail_plane,
                                             avail_planes.end(),
                                             [&dhl](const auto &plane) {
                                               return plane->Get()
                                                   ->IsValidForLayer(&dhl);
                                             });
    if (suitable_plane == avail_planes.end()) {
      return {};
    }
    plan->plan.emplace_back(std::move(dhl), *suitable_plane,
                            static_cast<int>(plan->plan.size()));

    first_avail_plane = suitable_plane + 1;
  }

  // Add cursor plane last to ensure it gets highest z-pos.
  if (cursor_layer) {
    // cursor_plane was already checked at the beginning of function.
    plan->plan.emplace_back(std::move(cursor_layer.value()), cursor_plane,
                            static_cast<int>(plan->plan.size()));
  }

  return plan;
}

}  // namespace android::drm_hwcomposer
