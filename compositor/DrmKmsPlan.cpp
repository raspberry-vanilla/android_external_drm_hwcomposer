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

#define LOG_TAG "drmhwc"

#include "DrmKmsPlan.h"

#include "drm/DrmDevice.h"
#include "drm/DrmPlane.h"
#include "utils/log.h"

namespace android {
auto DrmKmsPlan::CreateDrmKmsPlan(
    DrmDisplayPipeline &pipe, std::vector<LayerData> composition,
    std::optional<LayerData> cursor_layer) -> std::unique_ptr<DrmKmsPlan> {
  auto plan = std::make_unique<DrmKmsPlan>();

  auto [avail_planes, cursor_plane] = pipe.GetUsablePlanes();

  int z_pos = 0;
  if (cursor_layer.has_value()) {
    if (cursor_plane &&
        cursor_plane->Get()->IsValidForLayer(&cursor_layer.value())) {
      plan->plan.emplace_back(
          LayerToPlaneJoining{.layer = std::move(cursor_layer.value()),
                              .plane = cursor_plane,
                              .z_pos = z_pos++});
    } else {
      // Cursor layer can't use cursor plane, so let it match normally with
      // others.
      composition.push_back(std::move(cursor_layer.value()));
    }
  }

  for (auto &dhl : composition) {
    std::shared_ptr<BindingOwner<DrmPlane>> plane;
    /* Skip unsupported planes */
    do {
      if (avail_planes.empty()) {
        return {};
      }

      plane = *avail_planes.begin();
      avail_planes.erase(avail_planes.begin());
    } while (!plane->Get()->IsValidForLayer(&dhl));

    LayerToPlaneJoining joining = {
        .layer = std::move(dhl),
        .plane = plane,
        .z_pos = z_pos++,
    };

    plan->plan.emplace_back(std::move(joining));
  }

  return plan;
}

}  // namespace android
