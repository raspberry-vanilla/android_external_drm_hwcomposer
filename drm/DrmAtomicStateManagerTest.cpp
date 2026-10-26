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

#include "drm/DrmAtomicStateManager.h"

#include <gtest/gtest.h>

#include <drm/drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicCommitSink.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmAtomicCommitSink.h"
#include "drm/DrmColorspace.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmPlane.h"
#include "drm/DrmProperty.h"
#include "drm/DrmTestUtils.h"
#include "drm/DrmUnique.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

namespace {

constexpr uint32_t kOverlayPlaneId = 310;
constexpr uint32_t kOrphanConnectorId = 105;
constexpr uint32_t kOrphanEncoderId = 5;
constexpr uint32_t kTestFbId1 = 501;
constexpr uint32_t kTestFbId2 = 502;
constexpr uint32_t kTestFbId3 = 503;
constexpr uint32_t kStaleBootloaderFbId = 999;
constexpr uint16_t kTestWidth = 1920;
constexpr uint16_t kTestHeight = 1080;
constexpr uint32_t kTestVRefresh = 60;
constexpr int kRapidCreateDestroyCount = 16;

class RecordingAtomicStateManager : public DrmAtomicStateManager {
 public:
  explicit RecordingAtomicStateManager(DrmDisplayPipeline* pipe)
      : DrmAtomicStateManager(pipe) {
  }

  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs& args) override {
    recorded_args_.push_back(args);
    return DrmAtomicStateManager::GetAtomicModeReqForArgs(args);
  }

  const std::vector<AtomicCommitArgs>& GetRecordedArgs() const {
    return recorded_args_;
  }

 private:
  std::vector<AtomicCommitArgs> recorded_args_;
};

class FakeKernelDrmAtomicCommitSink : public DrmAtomicCommitSink {
 public:
  using CommitHandler = std::function<int(int fd, drmModeAtomicReqPtr req,
                                          uint32_t flags, void* user_data,
                                          size_t attempt)>;

  explicit FakeKernelDrmAtomicCommitSink(CommitHandler handler)
      : handler_(std::move(handler)) {
  }

  size_t GetTotalCalls() const {
    return total_calls_;
  }

  size_t GetNonBlockCalls() const {
    return nonblock_calls_;
  }

  size_t GetRetrySleepCount() const {
    return retry_sleep_count_;
  }

  uint32_t GetMaxRetrySleepUsec() const {
    return max_retry_sleep_usec_;
  }

 protected:
  int AtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags,
                   void* user_data) const override {
    const size_t attempt = total_calls_++;
    if ((flags & DRM_MODE_ATOMIC_NONBLOCK) != 0U) {
      ++nonblock_calls_;
    }
    const int err = handler_(fd, req, flags, user_data, attempt);
    if (err == -1 && errno != 0) {
      return -errno;
    }
    return err;
  }

  void RetrySleep(uint32_t usec) const override {
    ++retry_sleep_count_;
    max_retry_sleep_usec_ = std::max(max_retry_sleep_usec_, usec);
  }

 private:
  CommitHandler handler_;
  mutable size_t total_calls_ = 0;
  mutable size_t nonblock_calls_ = 0;
  mutable size_t retry_sleep_count_ = 0;
  mutable uint32_t max_retry_sleep_usec_ = 0;
};

class TestDrmDevice : public FakeDrmDevice {
 public:
  void RegisterFakeEnumProperty(
      uint32_t obj_id, const std::string& prop_name, uint32_t prop_id,
      uint64_t value, std::vector<std::pair<uint64_t, std::string>> enums) {
    fake_enum_properties_[{obj_id, prop_name}] = FakeEnumPropertyDef{
        .prop_id = prop_id,
        .value = value,
        .enums = std::move(enums),
    };
  }

  int GetProperty(uint32_t obj_id, uint32_t obj_type, const char* prop_name,
                  DrmProperty* property) const override {
    auto it = fake_enum_properties_.find({obj_id, std::string(prop_name)});
    if (it != fake_enum_properties_.end()) {
      drmModePropertyRes raw_prop{};
      raw_prop.prop_id = it->second.prop_id;
      raw_prop.flags = DRM_MODE_PROP_ENUM;
      std::strncpy(raw_prop.name, prop_name, DRM_PROP_NAME_LEN - 1);
      std::vector<drm_mode_property_enum> raw_enums(it->second.enums.size());
      for (size_t i = 0; i < it->second.enums.size(); ++i) {
        raw_enums[i].value = it->second.enums[i].first;
        std::strncpy(raw_enums[i].name, it->second.enums[i].second.c_str(),
                     DRM_PROP_NAME_LEN - 1);
      }
      raw_prop.count_enums = static_cast<int>(raw_enums.size());
      raw_prop.enums = raw_enums.data();
      property->Init(nullptr, obj_id, &raw_prop, it->second.value);
      return 0;
    }
    return FakeDrmDevice::GetProperty(obj_id, obj_type, prop_name, property);
  }

  void RegisterFakeRawConnector(
      uint32_t connector_id,
      uint32_t connector_type = DRM_MODE_CONNECTOR_DisplayPort) {
    fake_raw_connector_types_[connector_id] = connector_type;
  }

  DrmModeConnectorUnique GetConnector(uint32_t connector_id) const override {
    auto it = fake_raw_connector_types_.find(connector_id);
    if (it != fake_raw_connector_types_.end()) {
      return FakeDrmConnector::MakeFakeRawConnector(connector_id,
                                                    /*encoder_id=*/0,
                                                    it->second);
    }
    return FakeDrmDevice::GetConnector(connector_id);
  }

 private:
  struct FakeEnumPropertyDef {
    uint32_t prop_id{};
    uint64_t value{};
    std::vector<std::pair<uint64_t, std::string>> enums;
  };

  std::map<std::pair<uint32_t, std::string>, FakeEnumPropertyDef>
      fake_enum_properties_;
  std::map<uint32_t, uint32_t> fake_raw_connector_types_;
};

}  // namespace

struct DrmAtomicStateManagerTest : public ::testing::Test {
  void SetUp() override {
    fake_device = std::make_unique<TestDrmDevice>();
    fake_connector = fake_device->AddPipelineResources();
    auto overlay = std::make_unique<FakeDrmPlane>(*fake_device,
                                                  DRM_PLANE_TYPE_OVERLAY,
                                                  /*is_valid=*/true,
                                                  kOverlayPlaneId);
    overlay_plane = overlay.get();
    fake_device->AddPlane(std::move(overlay));
    pipeline = fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
    ASSERT_NE(pipeline, nullptr);
    state_manager = DrmAtomicStateManager::CreateInstance(pipeline.get());
    ASSERT_NE(state_manager, nullptr);
  }

  void TearDown() override {
    fake_connector = nullptr;
    overlay_plane = nullptr;
    state_manager.reset();
    pipeline.reset();
    fake_device.reset();
  }

  static DrmAtomicStateManager::DrmAtomicRequest* AsDrmRequest(
      const std::unique_ptr<AtomicRequest>& req) {
    void* raw = req.get();
    return static_cast<DrmAtomicStateManager::DrmAtomicRequest*>(raw);
  }

  static LayerData CreateTestLayerData(uint32_t fb_id = kTestFbId1,
                                       uint16_t width = kTestWidth,
                                       uint16_t height = kTestHeight) {
    LayerData layer{};
    layer.bi = BufferInfo{};
    layer.bi->width = width;
    layer.bi->height = height;
    layer.fb = std::make_shared<FakeFbIdHandle>(fb_id);
    layer.pi.display_frame.i_rect = IRect{0, 0, static_cast<int32_t>(width),
                                          static_cast<int32_t>(height)};
    layer.pi.source_crop.f_rect = FRect{0.0F, 0.0F, static_cast<float>(width),
                                        static_cast<float>(height)};
    return layer;
  }

  std::shared_ptr<LayerToPlaneJoiningPlan> CreateSinglePlanePlan(
      uint32_t fb_id = kTestFbId1) const {
    auto plan = std::make_shared<LayerToPlaneJoiningPlan>();
    plan->plan.emplace_back(CreateTestLayerData(fb_id), pipeline->primary_plane,
                            0);
    return plan;
  }

  std::unique_ptr<TestDrmDevice> fake_device;
  FakeDrmConnector* fake_connector = nullptr;
  FakeDrmPlane* overlay_plane = nullptr;
  std::shared_ptr<DrmDisplayPipeline> pipeline;
  std::unique_ptr<DrmAtomicStateManager> state_manager;
};

TEST_F(DrmAtomicStateManagerTest, InitialStateIsInactiveAndEmptyArgsNoOp) {
  EXPECT_FALSE(state_manager->IsActive());

  AtomicCommitArgs empty_args{};
  EXPECT_FALSE(empty_args.HasInputs());
  EXPECT_EQ(state_manager->GetAtomicModeReqForArgs(empty_args), nullptr);
}

TEST_F(DrmAtomicStateManagerTest, ModesetCommitActivatesDisplay) {
  AtomicCommitArgs args{};
  args.blocking = true;
  args.power_mode = PowerMode::kOn;
  args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight, kTestVRefresh);
  args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(args);
  ASSERT_NE(req, nullptr);

  state_manager->FinishPendingRequest(args, std::move(req));
  EXPECT_TRUE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest, DoubleSettingSamePowerModeIsFilteredOut) {
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(req));
  ASSERT_TRUE(state_manager->IsActive());

  // Requesting PowerMode::kOn again with no other inputs should be a no-op.
  AtomicCommitArgs redundant_on{};
  redundant_on.power_mode = PowerMode::kOn;
  EXPECT_EQ(state_manager->GetAtomicModeReqForArgs(redundant_on), nullptr);
  EXPECT_FALSE(redundant_on.power_mode.has_value());
}

TEST_F(DrmAtomicStateManagerTest,
       FrameUpdateCommitsAndDisablesPreviouslyUsedOverlayPlane) {
  const auto [usable_planes, cursor_plane] = pipeline->GetUsablePlanes();
  ASSERT_GE(usable_planes.size(), 2U);

  // Frame 1: Modeset with both primary and overlay planes active.
  auto two_plane_plan = std::make_shared<LayerToPlaneJoiningPlan>();
  two_plane_plan->plan.emplace_back(CreateTestLayerData(kTestFbId1),
                                    usable_planes[0], 0);
  two_plane_plan->plan.emplace_back(CreateTestLayerData(kTestFbId2),
                                    usable_planes[1], 1);

  AtomicCommitArgs frame1{};
  frame1.blocking = true;
  frame1.power_mode = PowerMode::kOn;
  frame1.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                          kTestVRefresh);
  frame1.composition = two_plane_plan;

  auto req1 = state_manager->GetAtomicModeReqForArgs(frame1);
  ASSERT_NE(req1, nullptr);
  state_manager->FinishPendingRequest(frame1, std::move(req1));
  EXPECT_TRUE(state_manager->IsActive());

  // Frame 2: Present only primary plane; overlay plane should be disabled.
  AtomicCommitArgs frame2{};
  frame2.blocking = true;
  frame2.composition = CreateSinglePlanePlan(kTestFbId3);

  auto req2 = state_manager->GetAtomicModeReqForArgs(frame2);
  ASSERT_NE(req2, nullptr);
  state_manager->FinishPendingRequest(frame2, std::move(req2));
  EXPECT_TRUE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest, PowerOffCommitDeactivatesDisplay) {
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto on_req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(on_req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(on_req));
  ASSERT_TRUE(state_manager->IsActive());

  AtomicCommitArgs off_args{};
  off_args.blocking = true;
  off_args.power_mode = PowerMode::kOff;

  auto off_req = state_manager->GetAtomicModeReqForArgs(off_args);
  ASSERT_NE(off_req, nullptr);
  state_manager->FinishPendingRequest(off_args, std::move(off_req));
  EXPECT_FALSE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest,
       TeardownAfterPowerOffIsNotSkippedAndClearsModeAndCrtc) {
  // Step 1: Activate display with a modeset.
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto on_req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(on_req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(on_req));
  ASSERT_TRUE(state_manager->IsActive());

  // Step 2: Power off without teardown (ACTIVE=0, but CRTC_ID and MODE_ID
  // remain bound).
  AtomicCommitArgs off_args{};
  off_args.blocking = true;
  off_args.power_mode = PowerMode::kOff;
  off_args.teardown = false;

  auto off_req = state_manager->GetAtomicModeReqForArgs(off_args);
  ASSERT_NE(off_req, nullptr);
  state_manager->FinishPendingRequest(off_args, std::move(off_req));
  ASSERT_FALSE(state_manager->IsActive());

  // Step 3: Teardown commit (power_mode = kOff, teardown = true) while already
  // powered off must not be filtered out by CheckDoubleSettingState, and must
  // clear connector CRTC_ID and CRTC MODE_ID.
  AtomicCommitArgs teardown_args{};
  teardown_args.blocking = true;
  teardown_args.power_mode = PowerMode::kOff;
  teardown_args.teardown = true;

  auto teardown_req = state_manager->GetAtomicModeReqForArgs(teardown_args);
  ASSERT_NE(teardown_req, nullptr);
  auto* drm_req = AsDrmRequest(teardown_req);
  const uint32_t conn_id = pipeline->connector->Get()->GetId();
  const uint32_t crtc_id = pipeline->crtc->Get()->GetId();
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(), conn_id,
                                      kFakeConnectorCrtcPropId),
            0ULL);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(), crtc_id,
                                      kFakeCrtcModePropId),
            0ULL);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(), crtc_id,
                                      kFakeCrtcOutFencePropId),
            std::nullopt);
}

TEST_F(DrmAtomicStateManagerTest,
       EmptyCompositionWhilePoweredOffDoesNotForceDisplayActive) {
  // Step 1: Activate display with a modeset.
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto on_req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(on_req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(on_req));
  ASSERT_TRUE(state_manager->IsActive());

  // Step 2: Power off with teardown (MODE_ID=0, CRTC_ID=0, ACTIVE=0).
  AtomicCommitArgs off_args{};
  off_args.blocking = true;
  off_args.power_mode = PowerMode::kOff;
  off_args.teardown = true;

  auto off_req = state_manager->GetAtomicModeReqForArgs(off_args);
  ASSERT_NE(off_req, nullptr);
  state_manager->FinishPendingRequest(off_args, std::move(off_req));
  ASSERT_FALSE(state_manager->IsActive());

  // Step 3: Commit an empty plane-disabling composition while powered off
  // (without display_mode or explicit power_mode). It must not force
  // PowerMode::kOn (which sets ACTIVE=1 with MODE_ID=0 and fails with -EINVAL).
  AtomicCommitArgs clear_planes_args{};
  clear_planes_args.composition = std::make_shared<LayerToPlaneJoiningPlan>();

  auto clear_req = state_manager->GetAtomicModeReqForArgs(clear_planes_args);
  EXPECT_NE(clear_planes_args.power_mode, PowerMode::kOn);
  if (clear_req != nullptr) {
    state_manager->FinishPendingRequest(clear_planes_args,
                                        std::move(clear_req));
  }
  EXPECT_FALSE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest,
       ModesetClearsOrphanedConnectorBoundToTargetCrtc) {
  const uint32_t crtc_id = pipeline->crtc->Get()->GetId();

  // Add a second connector on the same DrmDevice whose kernel CRTC_ID property
  // still references our pipeline's CRTC (e.g. from bootloader or an unclean
  // MST detach).
  auto orphan_connector = std::make_unique<
      FakeDrmConnector>(fake_device.get(), kOrphanEncoderId,
                        /*is_external=*/true, kFakeConnectorMmWidth,
                        kFakeConnectorMmHeight, kOrphanConnectorId);
  orphan_connector->SetConnected(false);
  orphan_connector->SetCrtcIdPropertyValue(crtc_id);
  fake_device->AddConnector(std::move(orphan_connector));

  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(req, nullptr);
  auto* drm_req = AsDrmRequest(req);

  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(),
                                      kOrphanConnectorId,
                                      kFakeConnectorCrtcPropId),
            0ULL);
}

TEST_F(DrmAtomicStateManagerTest,
       InitialModesetDisablesStalePlanesBoundToTargetCrtc) {
  const uint32_t crtc_id = pipeline->crtc->Get()->GetId();

  // Simulate the overlay plane being left active on our CRTC in kernel state
  // (e.g. from bootloader handover) before DrmAtomicStateManager's first
  // modeset.
  ASSERT_NE(overlay_plane, nullptr);
  overlay_plane->SetCrtcIdPropertyValue(crtc_id);
  overlay_plane->SetFbIdPropertyValue(kStaleBootloaderFbId);

  // Initial modeset uses only the primary plane.
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(req, nullptr);
  auto* drm_req = AsDrmRequest(req);

  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(),
                                      kOverlayPlaneId, kFakePlaneCrtcPropId),
            0ULL);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(),
                                      kOverlayPlaneId, kFakePlaneFbPropId),
            0ULL);
}

TEST_F(DrmAtomicStateManagerTest, RapidCreateAndDestroyDoesNotDeadlock) {
  for (int i = 0; i < kRapidCreateDestroyCount; ++i) {
    auto manager = DrmAtomicStateManager::CreateInstance(pipeline.get());
    ASSERT_NE(manager, nullptr);
    EXPECT_FALSE(manager->IsActive());
  }
}

TEST_F(DrmAtomicStateManagerTest,
       NonBlockingCommitFollowedByBlockingAndNonBlockingDoesNotAbort) {
  // 1. Initial blocking modeset (frames_staged_ = 0, frames_tracked_ = 0)
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(req0, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(req0));

  // 2. Non-blocking commit without present fence (frames_staged_ becomes 1,
  // while frames_tracked_ stays 0).
  AtomicCommitArgs nonblock1{};
  nonblock1.blocking = false;
  nonblock1.composition = CreateSinglePlanePlan(kTestFbId2);
  auto req1 = state_manager->GetAtomicModeReqForArgs(nonblock1);
  ASSERT_NE(req1, nullptr);
  state_manager->FinishPendingRequest(nonblock1, std::move(req1));

  // 3. Blocking commit resets frame_objects_ to size 1 and synchronizes
  // frames_tracked_ = frames_staged_.
  AtomicCommitArgs blocking2{};
  blocking2.blocking = true;
  blocking2.composition = CreateSinglePlanePlan(kTestFbId3);
  auto req2 = state_manager->GetAtomicModeReqForArgs(blocking2);
  ASSERT_NE(req2, nullptr);
  state_manager->FinishPendingRequest(blocking2, std::move(req2));

  // 4. Next non-blocking commit with a valid out_fence_address increments
  // frames_staged_ to 2 and wakes CleanupPriorFrameResources().
  int zero_fd = 0;
  const SharedFd non_owning_stdin(&zero_fd, [](const int*) {});
  const int fence_fd = DupFd(non_owning_stdin);
  ASSERT_GE(fence_fd, 0);

  AtomicCommitArgs nonblock3{};
  nonblock3.blocking = false;
  nonblock3.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req3 = state_manager->GetAtomicModeReqForArgs(nonblock3);
  ASSERT_NE(req3, nullptr);
  auto* drm_req3 = AsDrmRequest(req3);
  drm_req3->out_fence_address = fence_fd;
  state_manager->FinishPendingRequest(nonblock3, std::move(req3));
  state_manager->WaitLastFrame();
  EXPECT_TRUE(state_manager->IsActive());
}

TEST_F(DrmAtomicStateManagerTest,
       NonBlockingCommitOnEmptyFrameObjectsDoesNotAbort) {
  // Simulate DrmAtomicCommitSink::CommitFrame copying args by value so
  // GetAtomicModeReqForArgs sets power_mode = kOn on the copy, and then
  // CleanUpFailedRequest calling FinishPendingRequest with the original args
  // (blocking == false, power_mode == std::nullopt) when frame_objects_ is
  // still empty.
  AtomicCommitArgs req_args{};
  req_args.blocking = false;
  req_args.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req = state_manager->GetAtomicModeReqForArgs(req_args);
  ASSERT_NE(req, nullptr);

  int zero_fd = 0;
  const SharedFd non_owning_stdin(&zero_fd, [](const int*) {});
  const int fence_fd = DupFd(non_owning_stdin);
  ASSERT_GE(fence_fd, 0);

  auto* drm_req = AsDrmRequest(req);
  drm_req->out_fence_address = fence_fd;

  AtomicCommitArgs orig_args{};
  orig_args.blocking = false;
  orig_args.composition = req_args.composition;
  state_manager->FinishPendingRequest(orig_args, std::move(req));
  state_manager->WaitLastFrame();
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkPreservesNegativeErrnoFromDrmModeAtomicCommit) {
  DrmAtomicCommitSink sink_impl;
  AtomicCommitSink& sink = sink_impl;
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan(kTestFbId1);

  const CommitStatus test_status = sink.TestAtomicCommit(
      {{state_manager.get(), modeset_args}});
  EXPECT_FALSE(test_status.success);
  EXPECT_LT(test_status.error_code, 0);

  const auto exec_status = sink.ExecuteAtomicCommit(
      {{state_manager.get(), modeset_args}});
  EXPECT_FALSE(exec_status.IsSuccess());
  EXPECT_EQ(exec_status.GetStatus().error_code, test_status.error_code);
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkRetriesNonBlockingCommitOnEbusyWhenErrnoClobbered) {
  AtomicCommitArgs modeset0{};
  modeset0.blocking = true;
  modeset0.power_mode = PowerMode::kOn;
  modeset0.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                            kTestVRefresh);
  modeset0.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = state_manager->GetAtomicModeReqForArgs(modeset0);
  ASSERT_NE(req0, nullptr);
  state_manager->FinishPendingRequest(modeset0, std::move(req0));
  ASSERT_TRUE(state_manager->IsActive());

  // Simulate drmModeAtomicCommit returning -EBUSY while thread-local errno is
  // clobbered to EBADF (e.g. by ~ScopedTrace() or drmFree()) on both the
  // initial non-blocking commit (attempt 0) and a subsequent retry commit
  // (attempt 2), before succeeding on attempt 3.
  FakeKernelDrmAtomicCommitSink sink_impl(
      [](int /*fd*/, drmModeAtomicReqPtr /*req*/, uint32_t /*flags*/,
         void* /*user_data*/, size_t attempt) -> int {
        if (attempt == 0U) {
          errno = EBADF;
          return -EBUSY;
        }
        if (attempt == 1U) {
          errno = EBUSY;
          return -1;
        }
        if (attempt == 2U) {
          errno = EBADF;
          return -EBUSY;
        }
        errno = EBADF;
        return 0;
      });
  AtomicCommitSink& sink = sink_impl;

  AtomicCommitArgs nonblock_args{};
  nonblock_args.blocking = false;
  nonblock_args.composition = CreateSinglePlanePlan(kTestFbId2);

  const auto res = sink.ExecuteAtomicCommit(
      {{state_manager.get(), nonblock_args}});
  EXPECT_TRUE(res.IsSuccess());
  EXPECT_EQ(sink_impl.GetNonBlockCalls(), 4U);
  EXPECT_EQ(sink_impl.GetTotalCalls(), 4U);
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkStopsEbusyRetryOnNonEbusyError) {
  AtomicCommitArgs modeset0{};
  modeset0.blocking = true;
  modeset0.power_mode = PowerMode::kOn;
  modeset0.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                            kTestVRefresh);
  modeset0.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = state_manager->GetAtomicModeReqForArgs(modeset0);
  ASSERT_NE(req0, nullptr);
  state_manager->FinishPendingRequest(modeset0, std::move(req0));
  ASSERT_TRUE(state_manager->IsActive());

  // Non-blocking commit returning -EBUSY on attempt 0 and -EINVAL on attempt 1
  // (even if thread-local errno remains EBUSY) must stop the EBUSY retry loop
  // immediately after attempt 1 and return -EINVAL.
  FakeKernelDrmAtomicCommitSink sink_einval(
      [](int /*fd*/, drmModeAtomicReqPtr /*req*/, uint32_t flags,
         void* /*user_data*/, size_t attempt) -> int {
        if ((flags & DRM_MODE_ATOMIC_NONBLOCK) == 0U) {
          return 0;
        }
        if (attempt == 0U) {
          errno = EBUSY;
          return -EBUSY;
        }
        if (attempt == 1U) {
          errno = EBUSY;
          return -EINVAL;
        }
        return 0;
      });
  AtomicCommitSink& einval_ref = sink_einval;

  AtomicCommitArgs nonblock_args{};
  nonblock_args.blocking = false;
  nonblock_args.composition = CreateSinglePlanePlan(kTestFbId2);

  const auto einval_res = einval_ref.ExecuteAtomicCommit(
      {{state_manager.get(), nonblock_args}});
  EXPECT_FALSE(einval_res.IsSuccess());
  EXPECT_EQ(einval_res.GetStatus().error_code, -EINVAL);
  EXPECT_EQ(sink_einval.GetNonBlockCalls(), 2U);
  EXPECT_EQ(sink_einval.GetTotalCalls(), 3U);
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkDoesNotRetryBlockingCommitOnEbusy) {
  AtomicCommitArgs modeset0{};
  modeset0.blocking = true;
  modeset0.power_mode = PowerMode::kOn;
  modeset0.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                            kTestVRefresh);
  modeset0.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = state_manager->GetAtomicModeReqForArgs(modeset0);
  ASSERT_NE(req0, nullptr);
  state_manager->FinishPendingRequest(modeset0, std::move(req0));
  ASSERT_TRUE(state_manager->IsActive());

  FakeKernelDrmAtomicCommitSink sink_blocking(
      [](int /*fd*/, drmModeAtomicReqPtr /*req*/, uint32_t /*flags*/,
         void* /*user_data*/, size_t attempt) -> int {
        if (attempt == 0U) {
          errno = EBUSY;
          return -EBUSY;
        }
        return 0;
      });
  AtomicCommitSink& blocking_ref = sink_blocking;

  AtomicCommitArgs blocking_args{};
  blocking_args.blocking = true;
  blocking_args.composition = CreateSinglePlanePlan(kTestFbId3);

  const auto blocking_res = blocking_ref.ExecuteAtomicCommit(
      {{state_manager.get(), blocking_args}});
  EXPECT_FALSE(blocking_res.IsSuccess());
  EXPECT_EQ(blocking_res.GetStatus().error_code, -EBUSY);
  EXPECT_EQ(sink_blocking.GetNonBlockCalls(), 0U);
  EXPECT_EQ(sink_blocking.GetTotalCalls(), 2U);
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkSkipsPlaneCleanupOnFailedModeset) {
  RecordingAtomicStateManager recording_manager(pipeline.get());

  AtomicCommitArgs modeset0{};
  modeset0.blocking = true;
  modeset0.power_mode = PowerMode::kOn;
  modeset0.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                            kTestVRefresh);
  modeset0.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = recording_manager.GetAtomicModeReqForArgs(modeset0);
  ASSERT_NE(req0, nullptr);
  recording_manager.FinishPendingRequest(modeset0, std::move(req0));
  ASSERT_TRUE(recording_manager.IsActive());
  ASSERT_EQ(recording_manager.GetRecordedArgs().size(), 1U);

  // A failed modeset commit on an active display must not trigger a follow-up
  // plane-disable commit in CleanUpFailedRequest().
  DrmAtomicCommitSink sink_impl;
  AtomicCommitSink& sink = sink_impl;
  AtomicCommitArgs failed_modeset{};
  failed_modeset.blocking = true;
  failed_modeset.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                  kTestVRefresh);
  failed_modeset.composition = CreateSinglePlanePlan(kTestFbId2);

  const auto modeset_res = sink.ExecuteAtomicCommit(
      {{&recording_manager, failed_modeset}});
  EXPECT_FALSE(modeset_res.IsSuccess());
  EXPECT_EQ(recording_manager.GetRecordedArgs().size(), 2U);
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkCleansUpPlanesOnFailedPageflip) {
  RecordingAtomicStateManager recording_manager(pipeline.get());

  AtomicCommitArgs modeset0{};
  modeset0.blocking = true;
  modeset0.power_mode = PowerMode::kOn;
  modeset0.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                            kTestVRefresh);
  modeset0.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = recording_manager.GetAtomicModeReqForArgs(modeset0);
  ASSERT_NE(req0, nullptr);
  recording_manager.FinishPendingRequest(modeset0, std::move(req0));
  ASSERT_TRUE(recording_manager.IsActive());
  ASSERT_EQ(recording_manager.GetRecordedArgs().size(), 1U);

  // A failed regular composition commit (no display_mode, power_mode, or
  // teardown) on an active display must trigger CleanUpFailedRequest() with an
  // empty composition plan so prior release fences can signal.
  DrmAtomicCommitSink sink_impl;
  AtomicCommitSink& sink = sink_impl;
  AtomicCommitArgs failed_pageflip{};
  failed_pageflip.blocking = true;
  failed_pageflip.composition = CreateSinglePlanePlan(kTestFbId3);

  const auto pageflip_res = sink.ExecuteAtomicCommit(
      {{&recording_manager, failed_pageflip}});
  EXPECT_FALSE(pageflip_res.IsSuccess());
  ASSERT_EQ(recording_manager.GetRecordedArgs().size(), 3U);
  ASSERT_NE(recording_manager.GetRecordedArgs().back().composition, nullptr);
  EXPECT_TRUE(
      recording_manager.GetRecordedArgs().back().composition->plan.empty());
}

TEST_F(DrmAtomicStateManagerTest,
       DrmConnectorInitQueriesLinkStatusPropertyForResetOnModeset) {
  auto* fake_conn = fake_connector;
  ASSERT_NE(fake_conn, nullptr);
  const uint32_t conn_id = fake_conn->GetId();

  // Register mandatory connector properties ("DPMS", "CRTC_ID") and optional
  // "link-status" property (with value DRM_MODE_LINK_STATUS_BAD) on fake_device
  // so DrmConnector::Init() can query them via DrmDevice::GetProperty().
  fake_device->RegisterFakeObjectProperty(conn_id, "DPMS", kFakeDpmsPropId,
                                          DRM_MODE_PROP_ENUM, DRM_MODE_DPMS_ON);
  fake_device->RegisterFakeObjectProperty(conn_id, "CRTC_ID",
                                          kFakeConnectorCrtcPropId,
                                          DRM_MODE_PROP_OBJECT, 0);
  fake_device->RegisterFakeObjectProperty(conn_id, "link-status",
                                          kFakeLinkStatusPropId,
                                          DRM_MODE_PROP_ENUM,
                                          DRM_MODE_LINK_STATUS_BAD);

  ASSERT_TRUE(fake_conn->CallBaseInit());
  EXPECT_EQ(fake_conn->GetLinkStatusProperty().GetId(), kFakeLinkStatusPropId)
      << "DrmConnector::Init() failed to query optional 'link-status' property";

  // Verify that an initial full modeset resets link-status to
  // DRM_MODE_LINK_STATUS_GOOD (0) in the atomic request.
  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan();

  auto req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(req, nullptr);
  auto* drm_req = AsDrmRequest(req);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(), conn_id,
                                      kFakeLinkStatusPropId),
            static_cast<uint64_t>(DRM_MODE_LINK_STATUS_GOOD));
  state_manager->FinishPendingRequest(modeset_args, std::move(req));

  // Only full modesets (display_mode && !seamless) reset link-status to GOOD;
  // seamless refresh-rate switches and pageflips must omit link-status.
  constexpr uint32_t kSeamlessVRefresh = 120;
  AtomicCommitArgs seamless_args{};
  seamless_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                 kSeamlessVRefresh);
  seamless_args.seamless = true;
  auto seamless_req = state_manager->GetAtomicModeReqForArgs(seamless_args);
  ASSERT_NE(seamless_req, nullptr);
  EXPECT_EQ(GetAtomicReqPropertyValue(AsDrmRequest(seamless_req)
                                          ->property_set.get(),
                                      conn_id, kFakeLinkStatusPropId),
            std::nullopt);

  AtomicCommitArgs pageflip_args{};
  pageflip_args.composition = CreateSinglePlanePlan(kTestFbId2);
  auto pageflip_req = state_manager->GetAtomicModeReqForArgs(pageflip_args);
  ASSERT_NE(pageflip_req, nullptr);
  EXPECT_EQ(GetAtomicReqPropertyValue(AsDrmRequest(pageflip_req)
                                          ->property_set.get(),
                                      conn_id, kFakeLinkStatusPropId),
            std::nullopt);
}

TEST_F(DrmAtomicStateManagerTest,
       TeardownSkipsConnectorCrtcIdWhenConnectorHasVanished) {
  auto* fake_conn = fake_connector;
  ASSERT_NE(fake_conn, nullptr);
  const uint32_t conn_id = fake_conn->GetId();
  const uint32_t crtc_id = pipeline->crtc->Get()->GetId();

  AtomicCommitArgs modeset_args{};
  modeset_args.blocking = true;
  modeset_args.power_mode = PowerMode::kOn;
  modeset_args.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                                kTestVRefresh);
  modeset_args.composition = CreateSinglePlanePlan(kTestFbId1);
  auto modeset_req = state_manager->GetAtomicModeReqForArgs(modeset_args);
  ASSERT_NE(modeset_req, nullptr);
  state_manager->FinishPendingRequest(modeset_args, std::move(modeset_req));
  ASSERT_TRUE(state_manager->IsActive());

  // Simulate the dynamic MST connector being unregistered from the kernel
  // before HwcDisplay::Deinit() runs its teardown commit.
  fake_conn->MarkDeleted();

  AtomicCommitArgs teardown_args{};
  teardown_args.blocking = true;
  teardown_args.power_mode = PowerMode::kOff;
  teardown_args.teardown = true;
  teardown_args.composition = std::make_shared<LayerToPlaneJoiningPlan>();

  auto teardown_req = state_manager->GetAtomicModeReqForArgs(teardown_args);
  ASSERT_NE(teardown_req, nullptr);
  auto* drm_req = AsDrmRequest(teardown_req);
  EXPECT_FALSE(GetAtomicReqPropertyValue(drm_req->property_set.get(), conn_id,
                                         kFakeConnectorCrtcPropId)
                   .has_value())
      << "Teardown commit included CRTC_ID for a kernel-unregistered MST "
         "connector, which causes drmModeAtomicCommit to fail with -ENOENT";
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(), crtc_id,
                                      kFakeCrtcActivePropId),
            0U);
  EXPECT_EQ(GetAtomicReqPropertyValue(drm_req->property_set.get(), crtc_id,
                                      kFakeCrtcModePropId),
            0U);
}

TEST_F(DrmAtomicStateManagerTest,
       MstConnectorUpdateModesAndPropertiesRefreshesDynamicProperties) {
  auto* mst_conn = fake_device->AddPipelineResources(/*is_external=*/true);
  ASSERT_NE(mst_conn, nullptr);
  const uint32_t conn_id = mst_conn->GetId();
  fake_device->RegisterFakeRawConnector(conn_id,
                                        DRM_MODE_CONNECTOR_DisplayPort);
  constexpr uint32_t kInitialColorspacePropId = 44;
  constexpr uint32_t kInitialHdrMetadataPropId = 45;
  constexpr uint32_t kFakePathPropId = 46;
  constexpr uint64_t kMstPathBlobId = 77;
  constexpr uint64_t kInitialBt2020RgbEnumValue = 9;
  constexpr uint32_t kRecreatedColorspacePropId = 144;
  constexpr uint32_t kRecreatedHdrMetadataPropId = 145;
  constexpr uint64_t kRecreatedBt2020RgbEnumValue = 12;

  fake_device->RegisterFakeObjectProperty(conn_id, "DPMS", kFakeDpmsPropId,
                                          DRM_MODE_PROP_ENUM, DRM_MODE_DPMS_ON);
  fake_device->RegisterFakeObjectProperty(conn_id, "CRTC_ID",
                                          kFakeConnectorCrtcPropId,
                                          DRM_MODE_PROP_OBJECT, 0);
  fake_device->RegisterFakeObjectProperty(conn_id, "PATH", kFakePathPropId,
                                          DRM_MODE_PROP_BLOB, kMstPathBlobId);
  fake_device->RegisterFakeEnumProperty(conn_id, "Colorspace",
                                        kInitialColorspacePropId, 0,
                                        {{0, "Default"},
                                         {kInitialBt2020RgbEnumValue,
                                          "BT2020_RGB"}});
  fake_device->RegisterFakeObjectProperty(conn_id, "HDR_OUTPUT_METADATA",
                                          kInitialHdrMetadataPropId,
                                          DRM_MODE_PROP_BLOB, 0);

  ASSERT_TRUE(mst_conn->CallBaseInit());
  ASSERT_TRUE(mst_conn->IsExternal());
  ASSERT_TRUE(mst_conn->IsMst());
  EXPECT_EQ(mst_conn->GetColorspaceProperty().GetId(),
            kInitialColorspacePropId);
  EXPECT_EQ(mst_conn->GetColorspacePropertyValue(DrmColorspace::kBt2020Rgb),
            kInitialBt2020RgbEnumValue);
  EXPECT_EQ(mst_conn->GetHdrOutputMetadataProperty().GetId(),
            kInitialHdrMetadataPropId);

  // Dynamic DP MST branch device hotplugs can recreate connector properties
  // with new kernel property IDs and updated enum value tables.
  fake_device->RegisterFakeEnumProperty(conn_id, "Colorspace",
                                        kRecreatedColorspacePropId, 0,
                                        {{0, "Default"},
                                         {kRecreatedBt2020RgbEnumValue,
                                          "BT2020_RGB"}});
  fake_device->RegisterFakeObjectProperty(conn_id, "HDR_OUTPUT_METADATA",
                                          kRecreatedHdrMetadataPropId,
                                          DRM_MODE_PROP_BLOB, 0);

  EXPECT_EQ(mst_conn->CallBaseUpdateModesAndProperties(), 0);
  EXPECT_TRUE(mst_conn->IsExternal());
  EXPECT_EQ(mst_conn->GetColorspaceProperty().GetId(),
            kRecreatedColorspacePropId);
  EXPECT_EQ(mst_conn->GetColorspacePropertyValue(DrmColorspace::kBt2020Rgb),
            kRecreatedBt2020RgbEnumValue);
  EXPECT_EQ(mst_conn->GetHdrOutputMetadataProperty().GetId(),
            kRecreatedHdrMetadataPropId);
}

TEST_F(DrmAtomicStateManagerTest,
       DrmAtomicCommitSinkExitsWithFailureAfterMaxEbusyRetries) {
  constexpr size_t kExpectedMaxBusyRetries = 9375U;
  constexpr int kUnexpectedExtraRetryExitCode = 2;

  // Stop the fixture's background thread before forking in EXPECT_EXIT.
  state_manager.reset();
  RecordingAtomicStateManager threadless_manager(pipeline.get());

  AtomicCommitArgs modeset0{};
  modeset0.blocking = true;
  modeset0.power_mode = PowerMode::kOn;
  modeset0.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                            kTestVRefresh);
  modeset0.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req0 = threadless_manager.GetAtomicModeReqForArgs(modeset0);
  ASSERT_NE(req0, nullptr);
  threadless_manager.FinishPendingRequest(modeset0, std::move(req0));

  const FakeKernelDrmAtomicCommitSink* sink_ptr = nullptr;
  FakeKernelDrmAtomicCommitSink always_ebusy_sink(
      [&sink_ptr](int /*fd*/, drmModeAtomicReqPtr /*req*/, uint32_t /*flags*/,
                  void* /*user_data*/, size_t attempt) -> int {
        // Fail fast with a distinct exit code if CommitFrame exceeds
        // kMaxBusyRetries (9375).
        if (attempt > kExpectedMaxBusyRetries) {
          std::_Exit(kUnexpectedExtraRetryExitCode);
        }
        if (attempt == kExpectedMaxBusyRetries) {
          std::fprintf(stderr,
                       "ebusy_exhausted_at_attempt=%zu,sleep_count=%zu,"
                       "max_sleep_usec=%u\n",
                       attempt, sink_ptr->GetRetrySleepCount(),
                       sink_ptr->GetMaxRetrySleepUsec());
        }
        errno = EBUSY;
        return -EBUSY;
      });
  sink_ptr = &always_ebusy_sink;
  AtomicCommitSink& sink = always_ebusy_sink;

  AtomicCommitArgs nonblock_args{};
  nonblock_args.blocking = false;
  nonblock_args.composition = CreateSinglePlanePlan(kTestFbId2);

  EXPECT_EXIT(sink.ExecuteAtomicCommit({{&threadless_manager, nonblock_args}}),
              ::testing::ExitedWithCode(EXIT_FAILURE),
              "ebusy_exhausted_at_attempt=9375,sleep_count=9375,max_sleep_usec="
              "3200");
}

TEST_F(
    DrmAtomicStateManagerTest,
    CursorPlaneShiftsNegativeCrtcPositionAndClearsSrcOffsetAtLeftAndTopEdges) {
  constexpr uint32_t kCursorPlaneId = 320;
  constexpr uint16_t kCursorSize = 64;
  constexpr int32_t kClippedLeft = 24;
  constexpr int32_t kClippedTop = 16;
  constexpr int32_t kVisibleWidth = 40;
  constexpr int32_t kVisibleHeight = 48;
  constexpr float kSrcLeft = 24.0F;
  constexpr float kSrcTop = 16.0F;
  constexpr float kSrcRight = 64.0F;
  constexpr float kSrcBottom = 64.0F;
  constexpr uint64_t kFixedPointShift = 16;

  // DRM cursor planes require full unclipped dimensions with negative
  // CRTC_X/CRTC_Y offsets and zero SRC_X/SRC_Y when partially off-screen.
  FakeDrmPlane cursor_plane(*fake_device, DRM_PLANE_TYPE_CURSOR,
                            /*is_valid=*/true, kCursorPlaneId);
  LayerData layer = CreateTestLayerData(kTestFbId1, kCursorSize, kCursorSize);
  layer.pi.display_frame.i_rect = IRect{0, 0, kVisibleWidth, kVisibleHeight};
  layer.pi.source_crop.f_rect = FRect{kSrcLeft, kSrcTop, kSrcRight, kSrcBottom};

  DstRectInfo whole_display_rect{
      .i_rect = IRect{0, 0, kTestWidth, kTestHeight}};
  DrmModeAtomicReqUnique pset = MakeDrmModeAtomicReqUnique();
  ASSERT_NE(pset, nullptr);
  DrmModeUserPropertyBlobUnique damage_out;

  const uint32_t crtc_id = pipeline->crtc->Get()->GetId();
  EXPECT_EQ(cursor_plane.AtomicSetState(*pset, layer, /*zpos=*/1, crtc_id,
                                        whole_display_rect, damage_out),
            0);

  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcXPropId),
            static_cast<uint64_t>(static_cast<int64_t>(-kClippedLeft)));
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcYPropId),
            static_cast<uint64_t>(static_cast<int64_t>(-kClippedTop)));
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcWPropId),
            uint64_t{kCursorSize});
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcHPropId),
            uint64_t{kCursorSize});
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcXPropId),
            0U);
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcYPropId),
            0U);
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcWPropId),
            uint64_t{kCursorSize} << kFixedPointShift);
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcHPropId),
            uint64_t{kCursorSize} << kFixedPointShift);
}

TEST_F(
    DrmAtomicStateManagerTest,
    CursorPlaneRestoresUnclippedDimensionsAndCrtcOffsetsAtRightAndBottomEdges) {
  constexpr uint32_t kCursorPlaneId = 321;
  constexpr uint16_t kCursorSize = 64;
  constexpr int32_t kCursorLeft = 1900;
  constexpr int32_t kCursorTop = 1050;
  constexpr int32_t kClippedWidth = 20;
  constexpr int32_t kClippedHeight = 30;
  constexpr uint64_t kFixedPointShift = 16;

  // SurfaceFlinger clips the display_frame to screen boundaries when the cursor
  // extends beyond the right or bottom edges. DrmPlane::AtomicSetState must
  // restore the full unclipped buffer dimensions (CRTC_W/CRTC_H) at the given
  // CRTC_X/CRTC_Y position and unscale SRC_W/SRC_H so the hardware cursor
  // engine can display the visible portion without distortion.
  FakeDrmPlane cursor_plane(*fake_device, DRM_PLANE_TYPE_CURSOR,
                            /*is_valid=*/true, kCursorPlaneId);
  LayerData layer = CreateTestLayerData(kTestFbId1, kCursorSize, kCursorSize);
  layer.pi.display_frame.i_rect = IRect{kCursorLeft, kCursorTop,
                                        kCursorLeft + kClippedWidth,
                                        kCursorTop + kClippedHeight};
  layer.pi.source_crop.f_rect = FRect{0.0F, 0.0F,
                                      static_cast<float>(kClippedWidth),
                                      static_cast<float>(kClippedHeight)};

  DstRectInfo whole_display_rect{
      .i_rect = IRect{0, 0, kTestWidth, kTestHeight}};
  DrmModeAtomicReqUnique pset = MakeDrmModeAtomicReqUnique();
  ASSERT_NE(pset, nullptr);
  DrmModeUserPropertyBlobUnique damage_out;

  const uint32_t crtc_id = pipeline->crtc->Get()->GetId();
  EXPECT_EQ(cursor_plane.AtomicSetState(*pset, layer, /*zpos=*/1, crtc_id,
                                        whole_display_rect, damage_out),
            0);

  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcXPropId),
            uint64_t{kCursorLeft});
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcYPropId),
            uint64_t{kCursorTop});
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcWPropId),
            uint64_t{kCursorSize});
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneCrtcHPropId),
            uint64_t{kCursorSize});
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcXPropId),
            0U);
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcYPropId),
            0U);
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcWPropId),
            uint64_t{kCursorSize} << kFixedPointShift);
  EXPECT_EQ(GetAtomicReqPropertyValue(pset.get(), kCursorPlaneId,
                                      kFakePlaneSrcHPropId),
            uint64_t{kCursorSize} << kFixedPointShift);
}

TEST_F(DrmAtomicStateManagerTest,
       GetAtomicModeReqForArgsUsesPerConnectorAndPerPlaneCrtcPropertyIds) {
  constexpr uint32_t kConn1CrtcPropId = 101;
  constexpr uint32_t kPlane1CrtcPropId = 201;
  constexpr uint32_t kConn2CrtcPropId = 102;
  constexpr uint32_t kPlane2CrtcPropId = 202;

  auto* conn1 = fake_connector;
  ASSERT_NE(conn1, nullptr);
  DrmPlane* plane1 = pipeline->primary_plane->Get();
  ASSERT_NE(plane1, nullptr);
  InitFakeDrmProperty(conn1->GetCrtcIdProperty(), conn1->GetId(),
                      kConn1CrtcPropId, "CRTC_ID", DRM_MODE_PROP_OBJECT, 0);
  InitFakeDrmProperty(plane1->GetCrtcProperty(), plane1->GetId(),
                      kPlane1CrtcPropId, "CRTC_ID", DRM_MODE_PROP_OBJECT, 0);

  auto* conn2 = fake_device->AddPipelineResources(/*is_external=*/true);
  ASSERT_NE(conn2, nullptr);
  auto pipeline2 = fake_device->GetBackend().CreatePipeline(*conn2);
  ASSERT_NE(pipeline2, nullptr);
  DrmPlane* plane2 = pipeline2->primary_plane->Get();
  ASSERT_NE(plane2, nullptr);
  InitFakeDrmProperty(conn2->GetCrtcIdProperty(), conn2->GetId(),
                      kConn2CrtcPropId, "CRTC_ID", DRM_MODE_PROP_OBJECT, 0);
  InitFakeDrmProperty(plane2->GetCrtcProperty(), plane2->GetId(),
                      kPlane2CrtcPropId, "CRTC_ID", DRM_MODE_PROP_OBJECT, 0);

  auto state_manager2 = DrmAtomicStateManager::CreateInstance(pipeline2.get());
  ASSERT_NE(state_manager2, nullptr);

  AtomicCommitArgs args1{};
  args1.power_mode = PowerMode::kOn;
  args1.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                         kTestVRefresh);
  args1.composition = CreateSinglePlanePlan(kTestFbId1);
  auto req1 = state_manager->GetAtomicModeReqForArgs(args1);
  ASSERT_NE(req1, nullptr);

  AtomicCommitArgs args2{};
  args2.power_mode = PowerMode::kOn;
  args2.display_mode = CreateFakeDrmMode(kTestWidth, kTestHeight,
                                         kTestVRefresh);
  args2.composition = std::make_shared<LayerToPlaneJoiningPlan>();
  args2.composition->plan.emplace_back(CreateTestLayerData(kTestFbId2),
                                       pipeline2->primary_plane, 0);
  auto req2 = state_manager2->GetAtomicModeReqForArgs(args2);
  ASSERT_NE(req2, nullptr);

  EXPECT_EQ(GetAtomicReqPropertyValue(AsDrmRequest(req1)->property_set.get(),
                                      conn1->GetId(), kConn1CrtcPropId),
            pipeline->crtc->Get()->GetId());
  EXPECT_EQ(GetAtomicReqPropertyValue(AsDrmRequest(req1)->property_set.get(),
                                      plane1->GetId(), kPlane1CrtcPropId),
            pipeline->crtc->Get()->GetId());

  EXPECT_EQ(GetAtomicReqPropertyValue(AsDrmRequest(req2)->property_set.get(),
                                      conn2->GetId(), kConn2CrtcPropId),
            pipeline2->crtc->Get()->GetId());
  EXPECT_EQ(GetAtomicReqPropertyValue(AsDrmRequest(req2)->property_set.get(),
                                      plane2->GetId(), kPlane2CrtcPropId),
            pipeline2->crtc->Get()->GetId());
}

}  // namespace android::drm_hwcomposer
