/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "GenericBackend.h"

#include <memory>

#include "backend/Backend.h"
#include "backend/BackendManager.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/CompositionPlanner.h"
#include "compositor/GenericCompositionPlanner.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "drm/DrmAtomicCommitSink.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDisplayPipeline.h"
#include "utils/properties.h"

#if defined(USE_IMAPPER4_METADATA_API)
#include "bufferinfo/BufferInfoMapperMetadata.h"
#include "utils/log.h"
#endif

namespace android::drm_hwcomposer {

GenericBackend::GenericBackend(DrmDevice& drm) : Backend(drm) {
}

std::unique_ptr<DrmDisplayPipeline> GenericBackend::CreatePipeline(
    DrmConnector& connector) {
  auto pipeline = DrmDisplayPipeline::CreatePipeline(connector);
  if (pipeline) {
    pipeline->planner = CreateCompositionPlanner();
    pipeline->atomic_state_manager = DrmAtomicStateManager::CreateInstance(
        pipeline.get());
  }
  return pipeline;
}

std::unique_ptr<BufferInfoGetter> GenericBackend::CreateBufferInfoGetter() {
  std::unique_ptr<BufferInfoGetter> info_getter;
#if defined(USE_IMAPPER4_METADATA_API)
  info_getter = BufferInfoMapperMetadata::CreateInstance();
  if (!info_getter) {
    ALOGW("Generic buffer getter is not available. Falling back to legacy...");
  }
#endif
  if (!info_getter) {
    info_getter = LegacyBufferInfoGetter::CreateInstance();
  }
  return info_getter;
}

std::unique_ptr<CompositionPlanner> GenericBackend::CreateCompositionPlanner() {
  if (Properties::ForcedHolePunchingEnabled()) {
    return std::make_unique<GenericLayerMapperCompositionPlanner>();
  }

  return std::make_unique<GenericCompositionPlanner>();
}

std::unique_ptr<AtomicCommitSink> GenericBackend::CreateAtomicCommitSink() {
  return std::make_unique<DrmAtomicCommitSink>();
}

// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<GenericBackend> kRegisterGeneric(
    "generic");

}  // namespace android::drm_hwcomposer
