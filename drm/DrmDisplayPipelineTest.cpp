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

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include "DrmConnector.h"
#include "DrmCrtc.h"
#include "DrmDevice.h"
#include "DrmDisplayPipeline.h"
#include "DrmEncoder.h"
#include "DrmPlane.h"

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

class DrmDisplayPipelineTest : public ::testing::Test {
 public:
  void SetUp() override {
    fake_device_ = std::make_unique<FakeDrmDevice>();
  }

  void TearDown() override {
    fake_device_.reset();
  }

  std::unique_ptr<DrmDisplayPipeline> CreatePipeline() {
    fake_device_->AddPipelineResources();

    return DrmDisplayPipeline::CreatePipeline(
        *fake_device_->GetConnectors().back());
  }

  void AddPlane(uint32_t type) {
    fake_device_->AddPlane(std::make_unique<FakeDrmPlane>(*fake_device_, type));
  }

 private:
  std::unique_ptr<FakeDrmDevice> fake_device_ = nullptr;
};

TEST_F(DrmDisplayPipelineTest, BindPipeline_Success) {
  const auto pipeline = CreatePipeline();
  NoOpBindable bindable;

  const auto binding = bindable.BindPipeline(pipeline.get(),
                                             /*return_object_if_bound=*/true);

  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->Get()->GetPipeline(), pipeline.get());
  EXPECT_EQ(binding->Get(), &bindable);
}

TEST_F(DrmDisplayPipelineTest, BindPipeline_SamePipelineReturnsSameBinding) {
  const auto pipeline = CreatePipeline();
  NoOpBindable bindable;

  const auto binding1 = bindable.BindPipeline(pipeline.get(),
                                              /*return_object_if_bound=*/true);
  ASSERT_NE(binding1, nullptr);
  EXPECT_EQ(binding1->Get()->GetPipeline(), pipeline.get());
  EXPECT_EQ(binding1->Get(), &bindable);
  const auto binding2 = bindable.BindPipeline(pipeline.get(),
                                              /*return_object_if_bound=*/true);
  EXPECT_EQ(binding1, binding2);
}

TEST_F(DrmDisplayPipelineTest,
       BindPipeline_DifferentPipelineReturnsNullBinding) {
  const auto pipeline1 = CreatePipeline();
  const auto pipeline2 = CreatePipeline();
  NoOpBindable bindable;

  const auto binding1 = bindable.BindPipeline(pipeline1.get(),
                                              /*return_object_if_bound=*/true);
  ASSERT_NE(binding1, nullptr);
  EXPECT_EQ(binding1->Get()->GetPipeline(), pipeline1.get());
  EXPECT_EQ(binding1->Get(), &bindable);
  const auto binding2 = bindable.BindPipeline(pipeline2.get(),
                                              /*return_object_if_bound=*/true);
  EXPECT_EQ(binding2, nullptr);
}

TEST_F(DrmDisplayPipelineTest,
       BindPipeline_ReleasedBindingDoesntPreventNewBinding) {
  const auto pipeline1 = CreatePipeline();
  const auto pipeline2 = CreatePipeline();
  NoOpBindable bindable;

  auto binding1 = bindable.BindPipeline(pipeline1.get(),
                                        /*return_object_if_bound=*/true);
  ASSERT_NE(binding1, nullptr);
  EXPECT_EQ(binding1->Get()->GetPipeline(), pipeline1.get());
  EXPECT_EQ(binding1->Get(), &bindable);
  binding1.reset();
  const auto binding2 = bindable.BindPipeline(pipeline2.get(),
                                              /*return_object_if_bound=*/true);
  ASSERT_NE(binding2, nullptr);
  EXPECT_EQ(binding2->Get()->GetPipeline(), pipeline2.get());
  EXPECT_EQ(binding2->Get(), &bindable);
}

TEST_F(DrmDisplayPipelineTest, CreatePipeline_Success) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources();

  EXPECT_NE(DrmDisplayPipeline::CreatePipeline(
                *fake_device->GetConnectors().back()),
            nullptr);
}

TEST_F(DrmDisplayPipelineTest, CreatePipeline_TwoBindingSuccess) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources();

  const auto pipeline1 = DrmDisplayPipeline::CreatePipeline(
      *fake_device->GetConnectors().back());
  EXPECT_NE(pipeline1, nullptr);

  fake_device->AddPipelineResources();
  const auto pipeline2 = DrmDisplayPipeline::CreatePipeline(
      *fake_device->GetConnectors().back());
  EXPECT_NE(pipeline2, nullptr);
  EXPECT_NE(pipeline1, pipeline2);
}

TEST_F(DrmDisplayPipelineTest, CreatePipeline_AlreadyBoundFailure) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources();

  const auto pipeline1 = DrmDisplayPipeline::CreatePipeline(
      *fake_device->GetConnectors().back());
  EXPECT_NE(pipeline1, nullptr);

  EXPECT_DEATH(DrmDisplayPipeline::CreatePipeline(
                   *fake_device->GetConnectors().back()),
               "");
}

TEST_F(DrmDisplayPipelineTest, CreatePipeline_ReleasedBindingSuccess) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources();

  auto pipeline1 = DrmDisplayPipeline::CreatePipeline(
      *fake_device->GetConnectors().back());
  EXPECT_NE(pipeline1, nullptr);

  pipeline1.reset();

  EXPECT_NE(DrmDisplayPipeline::CreatePipeline(
                *fake_device->GetConnectors().back()),
            nullptr);
}

TEST_F(DrmDisplayPipelineTest, CreatePipeline_NoPrimaryPlaneFailure) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddEncoder(std::make_unique<FakeDrmEncoder>(/*id=*/0,
                                                           /*crtc_id=*/0));
  fake_device->AddCrtc(std::make_unique<FakeDrmCrtc>(/*id=*/0));
  fake_device->AddConnector(
      std::make_unique<FakeDrmConnector>(fake_device.get(),
                                         /*encoder_id=*/0));
  fake_device->AddPlane(
      std::make_unique<FakeDrmPlane>(*fake_device, DRM_PLANE_TYPE_OVERLAY));

  EXPECT_DEATH(DrmDisplayPipeline::CreatePipeline(
                   *fake_device->GetConnectors().back()),
               "Primary plane for CRTC 0 not found");
}

TEST_F(DrmDisplayPipelineTest, GetUsablePlanes_Success) {
  const auto pipeline = CreatePipeline();
  ASSERT_NE(pipeline, nullptr);

  AddPlane(DRM_PLANE_TYPE_OVERLAY);
  AddPlane(DRM_PLANE_TYPE_CURSOR);

  const auto [usable_planes, cursor_plane] = pipeline->GetUsablePlanes();
  EXPECT_EQ(usable_planes.size(), 2);
  EXPECT_NE(cursor_plane, nullptr);
}

TEST_F(DrmDisplayPipelineTest, GetUsablePlanes_SamePipeline) {
  const auto pipeline = CreatePipeline();
  ASSERT_NE(pipeline, nullptr);

  AddPlane(DRM_PLANE_TYPE_OVERLAY);
  AddPlane(DRM_PLANE_TYPE_CURSOR);

  const auto usable_planes_1 = pipeline->GetUsablePlanes();
  const auto usable_planes_2 = pipeline->GetUsablePlanes();
  EXPECT_EQ(usable_planes_1, usable_planes_2);
}

TEST_F(DrmDisplayPipelineTest, GetUsablePlanes_SharedPlanesAlreadyBound) {
  const auto pipeline1 = CreatePipeline();
  ASSERT_NE(pipeline1, nullptr);
  const auto pipeline2 = CreatePipeline();
  ASSERT_NE(pipeline2, nullptr);

  AddPlane(DRM_PLANE_TYPE_OVERLAY);
  AddPlane(DRM_PLANE_TYPE_CURSOR);

  const auto [usable_planes_1, cursor_plane_1] = pipeline1->GetUsablePlanes();
  EXPECT_EQ(usable_planes_1.size(), 2);
  EXPECT_NE(cursor_plane_1, nullptr);

  const auto [usable_planes_2, cursor_plane_2] = pipeline2->GetUsablePlanes();
  ASSERT_EQ(usable_planes_2.size(), 1);
  EXPECT_EQ(usable_planes_2[0]->Get()->GetType(), DRM_PLANE_TYPE_PRIMARY);
  EXPECT_EQ(cursor_plane_2, nullptr);
}

TEST_F(DrmDisplayPipelineTest, GetUsablePlanes_SharedPlaneBindingsReleased) {
  const auto pipeline1 = CreatePipeline();
  ASSERT_NE(pipeline1, nullptr);
  const auto pipeline2 = CreatePipeline();
  ASSERT_NE(pipeline2, nullptr);

  AddPlane(DRM_PLANE_TYPE_OVERLAY);
  AddPlane(DRM_PLANE_TYPE_CURSOR);

  {
    const auto [usable_planes, cursor_plane] = pipeline1->GetUsablePlanes();
    EXPECT_EQ(usable_planes.size(), 2);
    EXPECT_NE(cursor_plane, nullptr);
  }

  {
    const auto [usable_planes, cursor_plane] = pipeline2->GetUsablePlanes();
    EXPECT_EQ(usable_planes.size(), 2);
    EXPECT_NE(cursor_plane, nullptr);
  }
}

}  // namespace android::drm_hwcomposer
