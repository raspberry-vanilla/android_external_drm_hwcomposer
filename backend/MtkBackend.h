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

#include <memory>

#include "backend/Backend.h"
#include "backend/GenericBackend.h"

namespace android::drm_hwcomposer {

class CompositionPlanner;

class MtkBackend : public GenericBackend {
 public:
  explicit MtkBackend(DrmDevice& drm_device);
  ~MtkBackend() override = default;

 protected:
  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override;
};

}  // namespace android::drm_hwcomposer