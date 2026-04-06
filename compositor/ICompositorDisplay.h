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

#pragma once

#include <memory>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "drm/DrmDisplayPipeline.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

class DrmPlane;
class FlatteningController;

// ICompositorDisplay exists purely to isolate methods in HwcDisplay used inside
// the compositor/ directory. HwcDisplay has many Android-specific dependencies
// that prevent host-side unit tests from building, so this interface
// facilitates mocking of portion of HwcDisplay used in this directory.
class ICompositorDisplay {
 public:
  virtual ~ICompositorDisplay() = default;

  virtual std::vector<const HwcLayer *> GetOrderLayersByZPos() const = 0;

  virtual const FlatteningController *GetFlatCon() const = 0;

  virtual size_t GetNumAvailablePlanes() const = 0;
  virtual std::shared_ptr<BindingOwner<DrmPlane>> GetCursorPlane() const = 0;

  virtual bool TestComposition(
      CompositionPlanner::ValidatedComposition &composition) const = 0;

  virtual bool CtmByGpu() const = 0;
  virtual bool ForcedScalingWithGpu() const = 0;

  // Returns the currently configured display resolution as {width, height}.
  virtual std::pair<uint32_t, uint32_t> GetSize() const = 0;

  virtual const HwcLayer &GetClientLayer() const = 0;
};

}  // namespace android::drm_hwcomposer