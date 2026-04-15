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

#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmPlane.h"

namespace android::drm_hwcomposer {

class NoOpBindable : public PipelineBindable<NoOpBindable> {};

class FakeDrmConnector : public DrmConnector {
 public:
  FakeDrmConnector(DrmDevice* dev, uint32_t encoder_id)
      : DrmConnector(nullptr, dev, 0), encoder_id_(encoder_id) {
  }

  uint32_t GetCurrentEncoderId() const override {
    return encoder_id_;
  }

 private:
  const uint32_t encoder_id_;
};

class FakeDrmEncoder : public DrmEncoder {
 public:
  FakeDrmEncoder(uint32_t id, uint32_t crtc_id)
      : DrmEncoder(nullptr, 0), id_(id), crtc_id_(crtc_id) {
  }

  uint32_t GetId() const override {
    return id_;
  }

  uint32_t GetCurrentCrtcId() const override {
    return crtc_id_;
  }

 private:
  const uint32_t id_;
  const uint32_t crtc_id_;
};

class FakeDrmCrtc : public DrmCrtc {
 public:
  explicit FakeDrmCrtc(uint32_t id) : DrmCrtc(nullptr, 0), id_(id) {
  }

  uint32_t GetId() const override {
    return id_;
  }

 private:
  const uint32_t id_;
};

class FakeDrmPlane : public DrmPlane {
 public:
  FakeDrmPlane(DrmDevice& dev, uint32_t type) : DrmPlane(dev, nullptr) {
    type_ = type;
  }

  bool IsCrtcSupported(const DrmCrtc& crtc) const override {
    return true;
  }

  bool IsValidForLayer(const LayerData* /*layer*/) override {
    return is_valid_;
  }

  bool is_valid_ = true;
};

class FakeDrmDevice : public DrmDevice {
 public:
  FakeDrmDevice() : DrmDevice(nullptr, 0) {
  }

  void AddConnector(std::unique_ptr<DrmConnector> connector) {
    connectors_.emplace_back(std::move(connector));
  }

  void AddEncoder(std::unique_ptr<DrmEncoder> encoder) {
    encoders_.emplace_back(std::move(encoder));
  }

  void AddCrtc(std::unique_ptr<DrmCrtc> crtc) {
    crtcs_.emplace_back(std::move(crtc));
  }

  void AddPlane(std::unique_ptr<DrmPlane> plane) {
    planes_.emplace_back(std::move(plane));
  }

  void AddPipelineResources() {
    AddEncoder(std::make_unique<FakeDrmEncoder>(/*id=*/next_id_,
                                                /*crtc_id=*/next_id_));
    AddCrtc(std::make_unique<FakeDrmCrtc>(/*id=*/next_id_));
    AddConnector(std::make_unique<FakeDrmConnector>(this,
                                                    /*encoder_id=*/next_id_));
    AddPlane(std::make_unique<FakeDrmPlane>(*this, DRM_PLANE_TYPE_PRIMARY));
    ++next_id_;
  }

 private:
  uint32_t next_id_ = 0;
};

}  // namespace android::drm_hwcomposer
