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

#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmPlane.h"
#include "drm/DrmTestUtils.h"

namespace android::drm_hwcomposer {

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

TEST_F(DrmDisplayPipelineTest, BindPipelineSuccess) {
  const auto pipeline = CreatePipeline();
  NoOpBindable bindable;

  const auto binding = bindable.BindPipeline(pipeline.get(),
                                             /*return_object_if_bound=*/true);

  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->Get()->GetPipeline(), pipeline.get());
  EXPECT_EQ(binding->Get(), &bindable);
}

TEST_F(DrmDisplayPipelineTest, BindPipelineSamePipelineReturnsSameBinding) {
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
       BindPipelineDifferentPipelineReturnsNullBinding) {
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
       BindPipelineReleasedBindingDoesntPreventNewBinding) {
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

TEST_F(DrmDisplayPipelineTest, CreatePipelineSuccess) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources();

  EXPECT_NE(DrmDisplayPipeline::CreatePipeline(
                *fake_device->GetConnectors().back()),
            nullptr);
}

TEST_F(DrmDisplayPipelineTest, CreatePipelineTwoBindingSuccess) {
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

TEST_F(DrmDisplayPipelineTest, CreatePipelineAlreadyBoundFailure) {
  const auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources();

  const auto pipeline1 = DrmDisplayPipeline::CreatePipeline(
      *fake_device->GetConnectors().back());
  EXPECT_NE(pipeline1, nullptr);

  EXPECT_DEATH(DrmDisplayPipeline::CreatePipeline(
                   *fake_device->GetConnectors().back()),
               "");
}

TEST_F(DrmDisplayPipelineTest, CreatePipelineReleasedBindingSuccess) {
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

TEST_F(DrmDisplayPipelineTest, CreatePipelineNoPrimaryPlaneFailure) {
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

TEST_F(DrmDisplayPipelineTest, GetUsablePlanesSuccess) {
  const auto pipeline = CreatePipeline();
  ASSERT_NE(pipeline, nullptr);

  AddPlane(DRM_PLANE_TYPE_OVERLAY);
  AddPlane(DRM_PLANE_TYPE_CURSOR);

  const auto [usable_planes, cursor_plane] = pipeline->GetUsablePlanes();
  EXPECT_EQ(usable_planes.size(), 2);
  EXPECT_NE(cursor_plane, nullptr);
}

TEST_F(DrmDisplayPipelineTest, GetUsablePlanesSamePipeline) {
  const auto pipeline = CreatePipeline();
  ASSERT_NE(pipeline, nullptr);

  AddPlane(DRM_PLANE_TYPE_OVERLAY);
  AddPlane(DRM_PLANE_TYPE_CURSOR);

  const auto usable_planes_1 = pipeline->GetUsablePlanes();
  const auto usable_planes_2 = pipeline->GetUsablePlanes();
  EXPECT_EQ(usable_planes_1, usable_planes_2);
}

TEST_F(DrmDisplayPipelineTest, GetUsablePlanesSharedPlanesAlreadyBound) {
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

TEST_F(DrmDisplayPipelineTest, GetUsablePlanesSharedPlaneBindingsReleased) {
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
