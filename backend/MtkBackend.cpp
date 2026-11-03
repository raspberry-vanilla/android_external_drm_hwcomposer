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

#include "MtkBackend.h"

#include <memory>
#include <string>

#include "backend/Backend.h"
#include "backend/BackendManager.h"
#include "backend/GenericBackend.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/MtkMappingValidator.h"

namespace android::drm_hwcomposer {

MtkBackend::MtkBackend(DrmDevice& drm_device) : GenericBackend(drm_device) {
}

std::unique_ptr<CompositionPlanner> MtkBackend::CreateCompositionPlanner() {
  return std::make_unique<GenericLayerMapperCompositionPlanner>(
      MtkValidateMapping);
}

// Register the backend for Mtk DRM driver
// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<MtkBackend> kRegisterMediatek("mediatek");

}  // namespace android::drm_hwcomposer