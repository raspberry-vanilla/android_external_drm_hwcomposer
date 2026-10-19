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

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicCommitSink.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmAtomicCommitSink.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmPlane.h"
#include "drm/DrmTestUtils.h"
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

 private:
  CommitHandler handler_;
  mutable size_t total_calls_ = 0;
  mutable size_t nonblock_calls_ = 0;
};

}  // namespace

struct DrmAtomicStateManagerTest : public ::testing::Test {
  void SetUp() override {
    fake_device = std::make_unique<FakeDrmDevice>();
    fake_device->AddPipelineResources();
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

  std::unique_ptr<FakeDrmDevice> fake_device;
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

}  // namespace android::drm_hwcomposer
