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

#pragma once

#include "backend/BackendManager.h"
#include "compositor/CompositionPlanner.h"

namespace android::drm_hwcomposer {

class DrmConnector;
struct DrmDisplayPipeline;

// Implement the Backend interface on top of upstream drm uAPI.
class GenericBackend : public BackendManager::Backend {
 public:
  // Create a DrmDisplayPipeline using generic heuristics on top of upstream drm
  // uAPI.
  std::unique_ptr<DrmDisplayPipeline> CreatePipeline(
      DrmConnector& connector) override;

  // Create a default BufferInfoGetter. By default this will create a
  // BufferInfoMapperMetadata, and fall back to LegacyBufferInfoGetter if that
  // fails.
  std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter() override;

 protected:
  explicit GenericBackend(const std::string& name);

  // Create a new GenericCompositionPlanner. Subclasses can override
  // this to create different composition planners while using the default logic
  // to create a DrmDisplayPipeline.
  virtual std::unique_ptr<CompositionPlanner> CreateCompositionPlanner();

 private:
  GenericBackend();
  static GenericBackend instance;
};

}  // namespace android::drm_hwcomposer
