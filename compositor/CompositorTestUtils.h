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

#include <drm/drm_fourcc.h>
#include <gmock/gmock.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "compositor/CompositionPlanner.h"
#include "compositor/FlatteningController.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

class MockFbIdHandle : public IDrmFbIdHandle {
 public:
  MOCK_METHOD(uint32_t, GetFbId, (), (const));
};

class MockCompositorDisplay : public ICompositorDisplay {
 public:
  MOCK_METHOD(std::vector<const HwcLayer*>, GetOrderLayersByZPos, (), (const));
  MOCK_METHOD(const FlatteningController*, GetFlatCon, (), (const));
  MOCK_METHOD(size_t, GetNumAvailablePlanes, (), (const));
  MOCK_METHOD(std::shared_ptr<BindingOwner<DrmPlane>>, GetCursorPlane, (),
              (const));
  MOCK_METHOD(bool, TestComposition,
              (CompositionPlanner::ValidatedComposition&), (const));
  MOCK_METHOD(bool, CtmByGpu, (), (const));
  MOCK_METHOD(bool, ForcedScalingWithGpu, (), (const));
  MOCK_METHOD((std::pair<uint32_t, uint32_t>), GetSize, (), (const));
  MOCK_METHOD(const HwcLayer&, GetClientLayer, (), (const));
};

class FakeFlatteningController : public FlatteningController {
 public:
  FakeFlatteningController()
      : FlatteningController(1, FlatConCallbacks{.trigger = []() {}},
                             std::chrono::milliseconds(1)) {
  }

  bool ShouldFlatten() const override {
    return should_flatten_;
  }

  void SetShouldFlatten(bool should_flatten) {
    should_flatten_ = should_flatten;
  }

 private:
  bool should_flatten_ = false;
};

class CompositorTestUtils {
 public:
  static HwcLayer CreateLayer(ICompositorDisplay* display, IRect dest_ltrb,
                              uint32_t z_order, CompositionType type,
                              float alpha = 1.0F,
                              uint32_t buffer_format = DRM_FORMAT_RGBA8888);
};

}  // namespace android::drm_hwcomposer