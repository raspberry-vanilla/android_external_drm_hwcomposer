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
#include <optional>
#include <utility>

#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <drm/drm_mode.h>

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

TEST_F(DrmDisplayPipelineTest,
       UpdateModesAndPropertiesClearsConnectionAndModesWhenConnectorVanishes) {
  FakeDrmDevice dev;
  constexpr uint32_t kEncoderId = 10;
  constexpr uint32_t kMstConnId = 150;
  auto conn = std::make_unique<FakeDrmConnector>(&dev, kEncoderId,
                                                 /*is_external=*/true,
                                                 kFakeConnectorMmWidth,
                                                 kFakeConnectorMmHeight,
                                                 kMstConnId);
  conn->InitRawConnector(DRM_MODE_CONNECTOR_DisplayPort);
  conn->SetConnected(true);
  conn->SetModes({CreateFakeDrmMode()});
  ASSERT_TRUE(conn->IsConnected());
  ASSERT_EQ(conn->GetModes().size(), 1U);

  // When a dynamic MST connector is destroyed in the kernel,
  // drmModeGetConnector returns nullptr (-ENODEV). UpdateModesAndProperties()
  // must mark the connector disconnected and clear stale modes.
  EXPECT_NE(conn->CallBaseUpdateModesAndProperties(), 0);
  EXPECT_TRUE(conn->IsDeleted());
  EXPECT_FALSE(conn->IsConnected())
      << "DrmConnector::UpdateModesAndProperties() left connection == "
         "DRM_MODE_CONNECTED after drmModeGetConnector failed";
  EXPECT_TRUE(conn->GetModes().empty())
      << "DrmConnector::UpdateModesAndProperties() retained stale modes after "
         "drmModeGetConnector failed";
}

TEST_F(DrmDisplayPipelineTest,
       VirtualConnectorIsNotClassifiedAsInternalWhenPhysicalPanelExists) {
  constexpr uint32_t kEdpEncoderId = 10;
  constexpr uint32_t kVirtualEncoderId = 11;
  constexpr uint32_t kDpEncoderId = 12;
  constexpr uint32_t kEdpConnId = 100;
  constexpr uint32_t kVirtualConnId = 101;
  constexpr uint32_t kDpConnId = 102;

  // 1. Pure virtual device (e.g. Cuttlefish VKMS with only Virtual-1):
  // Virtual-1 is classified as internal.
  {
    FakeDrmDevice pure_virtual_dev;
    auto virtual_conn = std::make_unique<
        FakeDrmConnector>(&pure_virtual_dev, kVirtualEncoderId,
                          /*is_external=*/false, kFakeConnectorMmWidth,
                          kFakeConnectorMmHeight, kVirtualConnId);
    virtual_conn->InitRawConnector(DRM_MODE_CONNECTOR_VIRTUAL);
    virtual_conn->SetName("Virtual-1");
    auto* virtual_ptr = virtual_conn.get();
    pure_virtual_dev.AddConnector(std::move(virtual_conn));
    EXPECT_TRUE(virtual_ptr->IsInternal());
  }

  // 2. Physical internal panel (eDP-1) + Virtual-1 (e.g. msm_drm CWB):
  // Virtual-1 is NOT classified as internal.
  {
    FakeDrmDevice dev;
    auto edp_conn = std::make_unique<FakeDrmConnector>(&dev, kEdpEncoderId,
                                                       /*is_external=*/false,
                                                       kFakeConnectorMmWidth,
                                                       kFakeConnectorMmHeight,
                                                       kEdpConnId);
    edp_conn->InitRawConnector(DRM_MODE_CONNECTOR_eDP);
    auto* edp_ptr = edp_conn.get();
    dev.AddConnector(std::move(edp_conn));

    auto virtual_conn = std::make_unique<
        FakeDrmConnector>(&dev, kVirtualEncoderId,
                          /*is_external=*/false, kFakeConnectorMmWidth,
                          kFakeConnectorMmHeight, kVirtualConnId);
    virtual_conn->InitRawConnector(DRM_MODE_CONNECTOR_VIRTUAL);
    virtual_conn->SetName("Virtual-1");
    auto* virtual_ptr = virtual_conn.get();
    dev.AddConnector(std::move(virtual_conn));

    EXPECT_TRUE(edp_ptr->IsInternal());
    EXPECT_FALSE(virtual_ptr->IsInternal())
        << "DrmConnector::IsInternal() misclassified "
           "DRM_MODE_CONNECTOR_VIRTUAL as an internal panel when physical "
           "eDP-1 exists on DrmDevice";
  }

  // 3. Headless desktop with physical external connector (DP-1) + Virtual-1:
  // Virtual-1 is NOT classified as internal.
  {
    FakeDrmDevice external_dev;
    auto dp_conn = std::make_unique<FakeDrmConnector>(&external_dev,
                                                      kDpEncoderId,
                                                      /*is_external=*/true,
                                                      kFakeConnectorMmWidth,
                                                      kFakeConnectorMmHeight,
                                                      kDpConnId);
    dp_conn->InitRawConnector(DRM_MODE_CONNECTOR_DisplayPort);
    external_dev.AddConnector(std::move(dp_conn));

    auto virtual_conn = std::make_unique<
        FakeDrmConnector>(&external_dev, kVirtualEncoderId,
                          /*is_external=*/false, kFakeConnectorMmWidth,
                          kFakeConnectorMmHeight, kVirtualConnId);
    virtual_conn->InitRawConnector(DRM_MODE_CONNECTOR_VIRTUAL);
    virtual_conn->SetName("Virtual-1");
    auto* virtual_ptr = virtual_conn.get();
    external_dev.AddConnector(std::move(virtual_conn));

    EXPECT_FALSE(virtual_ptr->IsInternal());
  }
}

TEST_F(
    DrmDisplayPipelineTest,
    ResetConnectorsAndCrtcsDisablesActivePlanesAndSkipsVirtualCwbConnectors) {
  FakeDrmDevice dev;
  constexpr uint32_t kCrtcId = 200;
  constexpr uint32_t kVirtualCrtcId = 201;
  constexpr uint32_t kPrimaryPlaneId = 300;
  constexpr uint32_t kOverlayPlaneId = 301;
  constexpr uint32_t kIdlePlaneId = 302;
  constexpr uint32_t kVirtualPlaneId = 303;
  constexpr uint32_t kEdpEncoderId = 10;
  constexpr uint32_t kVirtualEncoderId = 11;
  constexpr uint32_t kEdpConnId = 100;
  constexpr uint32_t kVirtualCwbConnId = 101;
  constexpr uint64_t kStaleBootFbId = 999;
  constexpr uint64_t kStaleModeBlobId = 888;

  auto crtc = std::make_unique<FakeDrmCrtc>(kCrtcId, /*index=*/0);
  crtc->GetActiveProperty().UpdateValue(1);
  crtc->GetModeProperty().UpdateValue(kStaleModeBlobId);
  auto* crtc_ptr = crtc.get();
  dev.AddCrtc(std::move(crtc));

  auto virtual_crtc = std::make_unique<FakeDrmCrtc>(kVirtualCrtcId,
                                                    /*index=*/1);
  virtual_crtc->GetActiveProperty().UpdateValue(1);
  virtual_crtc->GetModeProperty().UpdateValue(kStaleModeBlobId);
  auto* virtual_crtc_ptr = virtual_crtc.get();
  dev.AddCrtc(std::move(virtual_crtc));

  auto edp_conn = std::make_unique<FakeDrmConnector>(&dev, kEdpEncoderId,
                                                     /*is_external=*/false,
                                                     kFakeConnectorMmWidth,
                                                     kFakeConnectorMmHeight,
                                                     kEdpConnId);
  edp_conn->InitRawConnector(DRM_MODE_CONNECTOR_eDP);
  edp_conn->SetCrtcIdPropertyValue(kCrtcId);
  auto* edp_conn_ptr = edp_conn.get();
  dev.AddConnector(std::move(edp_conn));

  auto virtual_cwb_conn = std::make_unique<
      FakeDrmConnector>(&dev, kVirtualEncoderId, /*is_external=*/false,
                        kFakeConnectorMmWidth, kFakeConnectorMmHeight,
                        kVirtualCwbConnId);
  virtual_cwb_conn->InitRawConnector(DRM_MODE_CONNECTOR_VIRTUAL);
  virtual_cwb_conn->SetName("Virtual-1");
  virtual_cwb_conn->SetCrtcIdPropertyValue(kVirtualCrtcId);
  dev.AddConnector(std::move(virtual_cwb_conn));

  auto primary_plane = std::make_unique<FakeDrmPlane>(dev,
                                                      DRM_PLANE_TYPE_PRIMARY,
                                                      /*is_valid=*/true,
                                                      kPrimaryPlaneId);
  primary_plane->SetCrtcIdPropertyValue(kCrtcId);
  primary_plane->SetFbIdPropertyValue(kStaleBootFbId);
  auto* primary_plane_ptr = primary_plane.get();
  dev.AddPlane(std::move(primary_plane));

  auto overlay_plane = std::make_unique<FakeDrmPlane>(dev,
                                                      DRM_PLANE_TYPE_OVERLAY,
                                                      /*is_valid=*/true,
                                                      kOverlayPlaneId);
  overlay_plane->SetCrtcIdPropertyValue(kCrtcId);
  overlay_plane->SetFbIdPropertyValue(kStaleBootFbId);
  auto* overlay_plane_ptr = overlay_plane.get();
  dev.AddPlane(std::move(overlay_plane));

  auto idle_plane = std::make_unique<FakeDrmPlane>(dev, DRM_PLANE_TYPE_OVERLAY,
                                                   /*is_valid=*/true,
                                                   kIdlePlaneId);
  idle_plane->SetCrtcIdPropertyValue(0);
  idle_plane->SetFbIdPropertyValue(0);
  dev.AddPlane(std::move(idle_plane));

  auto virtual_plane = std::make_unique<FakeDrmPlane>(dev,
                                                      DRM_PLANE_TYPE_PRIMARY,
                                                      /*is_valid=*/true,
                                                      kVirtualPlaneId);
  virtual_plane->SetCrtcIdPropertyValue(kVirtualCrtcId);
  virtual_plane->SetFbIdPropertyValue(kStaleBootFbId);
  auto* virtual_plane_ptr = virtual_plane.get();
  dev.AddPlane(std::move(virtual_plane));

  dev.ResetConnectorsAndCrtcs();

  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kPrimaryPlaneId,
                                             kFakePlaneCrtcPropId),
            0ULL);
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kPrimaryPlaneId,
                                             kFakePlaneFbPropId),
            0ULL);
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kOverlayPlaneId,
                                             kFakePlaneCrtcPropId),
            0ULL);
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kOverlayPlaneId,
                                             kFakePlaneFbPropId),
            0ULL);
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kIdlePlaneId,
                                             kFakePlaneCrtcPropId),
            std::nullopt)
      << "ResetConnectorsAndCrtcs() redundantly disabled an already-idle plane";
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kIdlePlaneId, kFakePlaneFbPropId),
            std::nullopt);
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kVirtualCwbConnId,
                                             kFakeConnectorCrtcPropId),
            std::nullopt)
      << "ResetConnectorsAndCrtcs() touched Virtual-1 CWB connector on a "
         "device with a physical internal eDP connector";
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kVirtualCrtcId,
                                             kFakeCrtcActivePropId),
            std::nullopt)
      << "ResetConnectorsAndCrtcs() disabled the CRTC bound to an active "
         "non-internal virtual CWB connector";
  EXPECT_EQ(dev.GetLastResetReqPropertyValue(kVirtualPlaneId,
                                             kFakePlaneCrtcPropId),
            std::nullopt);

  // Verify cached DrmProperty values are synchronized after reset.
  EXPECT_EQ(edp_conn_ptr->GetCrtcIdProperty().GetValue(), 0ULL);
  EXPECT_EQ(crtc_ptr->GetActiveProperty().GetValue(), 0ULL);
  EXPECT_EQ(crtc_ptr->GetModeProperty().GetValue(), 0ULL);
  EXPECT_EQ(primary_plane_ptr->GetCrtcProperty().GetValue(), 0ULL);
  EXPECT_EQ(primary_plane_ptr->GetFbProperty().GetValue(), 0ULL);
  EXPECT_EQ(overlay_plane_ptr->GetCrtcProperty().GetValue(), 0ULL);
  EXPECT_EQ(overlay_plane_ptr->GetFbProperty().GetValue(), 0ULL);
  EXPECT_EQ(virtual_crtc_ptr->GetActiveProperty().GetValue(), 1ULL);
  EXPECT_EQ(virtual_plane_ptr->GetCrtcProperty().GetValue(), kVirtualCrtcId);
}

}  // namespace android::drm_hwcomposer
