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

#include <memory>

#include "backend/BackendManager.h"
#include "backend/GenericBackend.h"
#include "compositor/CompositionPlanner.h"
#include "compositor/ICompositorDisplay.h"
#include "drm/DrmDevice.h"

namespace android::drm_hwcomposer {
namespace {
class ClientCompositionPlanner : public CompositionPlanner {
 public:
  auto ValidateDisplay(const ICompositorDisplay* display)
      -> ValidationResult override {
    return {.composition = GetFlattenedComposition(display
                                                       ->GetOrderLayersByZPos(),
                                                   FlattenReason::kNone),
            .short_circuited = false};
  }
};

class ClientBackend : public GenericBackend {
 public:
  explicit ClientBackend(DrmDevice& drm) : GenericBackend(drm) {
  }

  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override {
    return std::make_unique<ClientCompositionPlanner>();
  }
};

// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<ClientBackend> kRegisterClient("client");

}  // namespace
}  // namespace android::drm_hwcomposer
