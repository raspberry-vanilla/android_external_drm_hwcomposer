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

#include "backend/GenericBackend.h"
#include "compositor/CompositionPlanner.h"
#include "hwc/HwcDisplay.h"

namespace android::drm_hwcomposer {
namespace {
class ClientCompositionPlanner : public CompositionPlanner {
 public:
  auto ValidateDisplay(const ICompositorDisplay* display)
      -> ValidatedComposition override {
    return GetFlattenedComposition(display->GetOrderLayersByZPos(),
                                   FlattenReason::kNone);
  }
};

class ClientBackend : public GenericBackend {
 public:
  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override {
    return std::make_unique<ClientCompositionPlanner>();
  }

 private:
  ClientBackend() : GenericBackend("client") {
  }

  static ClientBackend instance;
};

// NOLINTNEXTLINE(cert-err58-cpp)
ClientBackend ClientBackend::instance;

}  // namespace
}  // namespace android::drm_hwcomposer
