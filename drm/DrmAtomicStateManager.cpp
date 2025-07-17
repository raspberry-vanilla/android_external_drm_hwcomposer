/*
 * Copyright (C) 2015 The Android Open Source Project
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

#undef NDEBUG /* Required for assert to work */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "drmhwc"

#include "DrmAtomicStateManager.h"

#include <drm/drm_mode.h>
#include <sync/sync.h>
#include <utils/Trace.h>

#include <cassert>

#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmPlane.h"
#include "drm/DrmUnique.h"
#include "utils/log.h"

namespace android {

auto DrmAtomicStateManager::CreateInstance(DrmDisplayPipeline *pipe)
    -> std::shared_ptr<DrmAtomicStateManager> {
  auto dasm = std::shared_ptr<DrmAtomicStateManager>(
      new DrmAtomicStateManager());

  dasm->pipe_ = pipe;
  dasm->thread_ = std::thread(&DrmAtomicStateManager::ThreadFn, dasm.get());

  return dasm;
}

DrmAtomicStateManager::~DrmAtomicStateManager() {
  StopThread();
  thread_.join();
}

void DrmAtomicStateManager::WaitLastFrame() {
  SharedFd present_fence;
  {
    std::lock_guard lock(mutex_);
    present_fence = last_present_fence_;
  }

  if (present_fence) {
    // NOLINTNEXTLINE(misc-const-correctness)
    ATRACE_NAME("WaitPriorFramePresented");

    constexpr int kTimeoutMs = 500;
    const int err = sync_wait(*present_fence, kTimeoutMs);
    if (err != 0) {
      ALOGE("sync_wait(fd=%i) returned: %i (errno: %i)", *present_fence, err,
            errno);
    }

    // Lock again in case the helper thread cleaned this up while sync_waiting.
    std::lock_guard lock(mutex_);
    if (last_present_fence_) {
      CleanupPriorFrameResources();
    }
  }
}

void DrmAtomicStateManager::CleanFailedCommit() {
  // Disable the hw used by the last active composition. This allows us to
  // signal the release fences from that composition to avoid hanging.
  AtomicCommitArgs cl_args{};
  cl_args.composition = std::make_shared<DrmKmsPlan>();
  if (CommitFrame(cl_args) != 0) {
    ALOGE("Failed to clean-up active composition for pipeline %s",
          pipe_->connector->Get()->GetName().c_str());
  }
}

// NOLINTNEXTLINE (readability-function-cognitive-complexity): Fixme
auto DrmAtomicStateManager::CommitFrame(AtomicCommitArgs &args) -> int {
  // NOLINTNEXTLINE(misc-const-correctness)
  ATRACE_CALL();
  // new_frame_state is initialized to the current frame state and may be
  // modified below.
  args.new_frame_state = committed_frame_state_;
  args.used_kms_objects = {};

  CheckDoubleSettingState(args, args.new_frame_state.crtc_active_state);

  if (!args.HasInputs()) {
    /* nothing to do */
    return 0;
  }

  auto pset = GetAtomicModeReqForArgs(args);

  if (!pset) {
    ALOGE("Failed to get property set");
    return -ENOMEM;
  }

  uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  const int error_buf_max_size = 64;
  char err_buf[error_buf_max_size];
  auto *drm = pipe_->device;

  if (args.test_only) {
    auto err = drmModeAtomicCommit(*drm->GetFd(), pset.get(),
                                   flags | DRM_MODE_ATOMIC_TEST_ONLY, drm);

    ALOGE_IF(err != 0, "Test-only ret=%d errno=%d strerror=%s\n", err, errno,
             strerror_r(errno, err_buf, error_buf_max_size));
    return err;
  }

  WaitLastFrame();

  bool nonblock = !args.blocking && !args.active;

  if (nonblock) {
    flags |= DRM_MODE_ATOMIC_NONBLOCK;
  }

  auto err = drmModeAtomicCommit(*drm->GetFd(), pset.get(), flags, drm);

  if (err != 0) {
    ALOGE("Failed to commit pset ret=%d errno=%d strerror=%s\n", err, errno,
          strerror_r(errno, err_buf, error_buf_max_size));
    return err;
  }

  args.out_fence = MakeSharedFd(args.out_fence_address);

  // Store the writeback fence if this operation used a writeback connector
  if (pipe_->writeback_connector && args.writeback_fb) {
    args.out_writeback_complete_fence = MakeSharedFd(args.wb_fence_address);
  }

  committed_frame_state_ = std::move(args.new_frame_state);

  if (nonblock) {
    {
      const std::lock_guard lock(mutex_);
      last_present_fence_ = args.out_fence;
      frame_objects_.emplace(std::move(args.used_kms_objects));
      frames_staged_++;
    }
    cv_.notify_all();
  } else {
    const std::lock_guard lock(mutex_);
    last_present_fence_ = {};
    frame_objects_ = {};
    frame_objects_.emplace(std::move(args.used_kms_objects));
  }

  return 0;
}

void DrmAtomicStateManager::CheckDoubleSettingState(AtomicCommitArgs &args,
                                                    bool crtc_is_active) {
  if (args.active && *args.active == crtc_is_active) {
    /* Don't set the same state twice */
    args.active.reset();
  }
}

bool DrmAtomicStateManager::SetWriteBackFenceIfNeeded(drmModeAtomicReq *pset,
                                                      AtomicCommitArgs &args) {
  if (!pipe_->writeback_connector || !args.writeback_fb) {
    return true;
  }
  auto *crtc = pipe_->crtc->Get();

  if (!pipe_->writeback_connector->Get()
           ->GetCrtcIdProperty()
           .AtomicSet(*pset, crtc->GetId())) {
    ALOGE("DrmAtomicStateManager: Failed to set writeback CRTC_ID property");
    return false;
  }

  if (!pipe_->writeback_connector->Get()
           ->GetWritebackFbIdProperty()
           .AtomicSet(*pset, args.writeback_fb->GetFbId())) {
    ALOGE("DrmAtomicStateManager: Failed to set writeback FB_ID property");
    return false;
  }

  if (!pipe_->writeback_connector->Get()
           ->GetWritebackOutFenceProperty()
           .AtomicSet(*pset, uint64_t(&args.wb_fence_address))) {
    ALOGE(
        "DrmAtomicStateManager: Failed to set writeback OUT_FENCE_PTR "
        "property");
    return false;
  }

  // Wait on input fence if provided
  if (args.writeback_release_fence) {
    sync_wait(*args.writeback_release_fence, -1);
    args.writeback_release_fence.reset();
  }

  return true;
}

bool DrmAtomicStateManager::SetOutputFence(drmModeAtomicReq *pset,
                                           AtomicCommitArgs &args) {
  auto *crtc = pipe_->crtc->Get();

  return crtc->GetOutFencePtrProperty().AtomicSet(*pset,
                                                  uint64_t(
                                                      &args.out_fence_address));
}

bool DrmAtomicStateManager::SetActiveIfNeeded(drmModeAtomicReq *pset,
                                              AtomicCommitArgs &args) {
  if (!args.active) {
    return true;
  }
  auto *crtc = pipe_->crtc->Get();
  auto *connector = pipe_->connector->Get();
  args.new_frame_state.crtc_active_state = *args.active;
  if (!crtc->GetActiveProperty().AtomicSet(*pset, *args.active ? 1 : 0) ||
      !connector->GetCrtcIdProperty().AtomicSet(*pset, crtc->GetId())) {
    return false;
  }
  if (!*args.active && args.teardown) {
    if (!connector->GetCrtcIdProperty().AtomicSet(*pset, 0) ||
        !crtc->GetModeProperty().AtomicSet(*pset, 0)) {
      return false;
    }
  }

  return true;
}

bool DrmAtomicStateManager::SetDisplayModeIfNeeded(drmModeAtomicReq *pset,
                                                   AtomicCommitArgs &args) {
  if (!args.display_mode) {
    return true;
  }

  auto *drm = pipe_->device;
  auto mode_blob = args.display_mode.value().CreateModeBlob(*drm);

  if (!mode_blob) {
    ALOGE("Failed to create mode_blob");
    return false;
  }

  auto raw_mode = args.display_mode.value().GetRawMode();
  whole_display_rect_.i_rect = {0, 0, raw_mode.hdisplay, raw_mode.vdisplay};

  auto *crtc = pipe_->crtc->Get();
  if (!crtc->GetModeProperty().AtomicSet(*pset, *mode_blob)) {
    return false;
  }
  args.used_kms_objects.blobs.emplace_back(std::move(mode_blob));
  return true;
}

bool DrmAtomicStateManager::SetCtmIfNeeded(drmModeAtomicReq *pset,
                                           AtomicCommitArgs &args) {
  auto *crtc = pipe_->crtc->Get();
  if (!args.color_matrix || !crtc->GetCtmProperty()) {
    return true;
  }

  auto *drm = pipe_->device;
  auto ctm_blob = drm->RegisterUserPropertyBlob(args.color_matrix.get(),
                                                sizeof(drm_color_ctm));
  if (!ctm_blob) {
    ALOGE("Failed to create CTM blob");
    return false;
  }

  if (!crtc->GetCtmProperty().AtomicSet(*pset, *ctm_blob)) {
    return false;
  }

  args.used_kms_objects.blobs.emplace_back(std::move(ctm_blob));
  return true;
}

bool DrmAtomicStateManager::SetColorSpaceIfNeeded(drmModeAtomicReq *pset,
                                                  AtomicCommitArgs &args) {
  auto *connector = pipe_->connector->Get();
  if (!args.colorspace || !connector->GetColorspaceProperty()) {
    return true;
  }

  return connector->GetColorspaceProperty()
      .AtomicSet(*pset,
                 connector->GetColorspacePropertyValue(*args.colorspace));
}

bool DrmAtomicStateManager::SetContentTypeIfNeeded(drmModeAtomicReq *pset,
                                                   AtomicCommitArgs &args) {
  auto *connector = pipe_->connector->Get();
  if (!args.content_type || !connector->GetContentTypeProperty()) {
    return true;
  }
  return connector->GetContentTypeProperty().AtomicSet(*pset,
                                                       static_cast<uint64_t>(
                                                           *args.content_type));
}

bool DrmAtomicStateManager::SetHdrMetadataIfNeeded(drmModeAtomicReq *pset,
                                                   AtomicCommitArgs &args) {
  auto *connector = pipe_->connector->Get();
  if (!args.hdr_metadata || !connector->GetHdrOutputMetadataProperty()) {
    return true;
  }

  auto *drm = pipe_->device;
  auto hdr_metadata_blob = drm->RegisterUserPropertyBlob(
      args.hdr_metadata.get(), sizeof(hdr_output_metadata));
  if (!hdr_metadata_blob) {
    ALOGE("Failed to create %s blob",
          connector->GetHdrOutputMetadataProperty().GetName().c_str());
    return false;
  }

  if (!connector->GetHdrOutputMetadataProperty()
           .AtomicSet(*pset, *hdr_metadata_blob)) {
    return false;
  }
  args.used_kms_objects.blobs.emplace_back(std::move(hdr_metadata_blob));

  return true;
}

bool DrmAtomicStateManager::SetMinBpcIfNeeded(drmModeAtomicReq *pset,
                                              AtomicCommitArgs &args) {
  auto *connector = pipe_->connector->Get();
  if (!args.min_bpc || !connector->GetMinBpcProperty()) {
    return true;
  }

  int err = 0;
  uint64_t range_min = 0;
  uint64_t range_max = 0;
  std::tie(err, range_min) = connector->GetMinBpcProperty().RangeMin();
  if (err != 0) {
    return false;
  }

  std::tie(err, range_max) = connector->GetMinBpcProperty().RangeMax();
  if (err != 0) {
    return false;
  }

  // Adjust requested min bpc to be within the property range
  int32_t min_bpc_val = std::max(args.min_bpc.value(),
                                 static_cast<int32_t>(range_min));
  min_bpc_val = std::min(min_bpc_val, static_cast<int32_t>(range_max));
  return connector->GetMinBpcProperty().AtomicSet(*pset, min_bpc_val);
}

bool DrmAtomicStateManager::SetCompositionIfNeeded(drmModeAtomicReq *pset,
                                                   AtomicCommitArgs &args) {
  if (!args.composition) {
    return true;
  }

  auto unused_planes = args.new_frame_state.used_planes;
  args.new_frame_state.used_planes.clear();

  for (auto &joining : args.composition->plan) {
    DrmPlane *plane = joining.plane->Get();
    LayerData &layer = joining.layer;

    args.used_kms_objects.framebuffers.emplace_back(layer.fb);
    args.new_frame_state.used_planes.emplace_back(joining.plane);

    /* Remove from 'unused' list, since plane is re-used */
    auto &v = unused_planes;
    v.erase(std::remove(v.begin(), v.end(), joining.plane), v.end());

    DrmModeUserPropertyBlobUnique damage_blob;
    auto *crtc = pipe_->crtc->Get();
    if (plane->AtomicSetState(*pset, layer, joining.z_pos, crtc->GetId(),
                              whole_display_rect_, damage_blob) != 0) {
      return false;
    }
    args.used_kms_objects.blobs.emplace_back(std::move(damage_blob));
  }

  return std::all_of(unused_planes.begin(), unused_planes.end(),
                     [&pset](auto &plane) {
                       return plane->Get()->AtomicDisablePlane(*pset) == 0;
                     });
}

DrmModeAtomicReqUnique DrmAtomicStateManager::GetAtomicModeReqForArgs(
    AtomicCommitArgs &args) {
  ATRACE_CALL();
  if (!args.new_frame_state.crtc_active_state) {
    /* Force activate display */
    args.active = true;
  }

  auto pset = MakeDrmModeAtomicReqUnique();
  if (!pset) {
    ALOGE("Failed to allocate property set");
    return nullptr;
  }

  if (!SetWriteBackFenceIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set writeback fence");
    return nullptr;
  }

  if (!SetOutputFence(pset.get(), args)) {
    ALOGE("Failed to set output fence");
    return nullptr;
  }

  if (!SetActiveIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set active");
    return nullptr;
  }

  if (!SetDisplayModeIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set display mode");
    return nullptr;
  }

  if (!SetCtmIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set CTM blob");
    return nullptr;
  }

  if (!SetColorSpaceIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set color space");
    return nullptr;
  }

  if (!SetContentTypeIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set content type");
    return nullptr;
  }

  if (!SetHdrMetadataIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set HDR metadata");
    return nullptr;
  }

  if (!SetMinBpcIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set min BPC");
    return nullptr;
  }

  if (!SetCompositionIfNeeded(pset.get(), args)) {
    ALOGE("Failed to set composition");
    return nullptr;
  }

  return pset;
}

void DrmAtomicStateManager::ThreadFn() {
  int tracking_at_the_moment = -1;

  for (;;) {
    SharedFd present_fence;

    {
      std::unique_lock lk(mutex_);
      base::ScopedLockAssertion lock_assertion(mutex_);
      cv_.wait(lk);

      if (exit_thread_)
        break;

      if (frames_staged_ <= tracking_at_the_moment)
        continue;

      tracking_at_the_moment = frames_staged_;

      present_fence = last_present_fence_;
      if (!present_fence)
        continue;
    }

    {
      // NOLINTNEXTLINE(misc-const-correctness)
      ATRACE_NAME("AsyncWaitForBuffersSwap");
      constexpr int kTimeoutMs = 500;
      auto err = sync_wait(*present_fence, kTimeoutMs);
      if (err != 0) {
        ALOGE("sync_wait(fd=%i) returned: %i (errno: %i)", *present_fence, err,
              errno);
      }
    }

    {
      const std::lock_guard lk(mutex_);
      if (exit_thread_)
        break;

      /* If resources is already cleaned-up by main thread, skip */
      if (tracking_at_the_moment > frames_tracked_)
        CleanupPriorFrameResources();
    }
  }

  ALOGI("DrmAtomicStateManager thread exit");
}

void DrmAtomicStateManager::CleanupPriorFrameResources() {
  assert(frames_staged_ - frames_tracked_ == 1);
  assert(last_present_fence_);
  assert(frame_objects_.size() > 1);

  // NOLINTNEXTLINE(misc-const-correctness)
  ATRACE_NAME("CleanupPriorFrameResources");
  frames_tracked_++;
  frame_objects_.pop();
  last_present_fence_ = {};
}

auto DrmAtomicStateManager::ExecuteAtomicCommit(AtomicCommitArgs &args) -> int {
  auto err = CommitFrame(args);

  if (!args.test_only) {
    if (err != 0) {
      ALOGE("Composite failed for pipeline %s",
            pipe_->connector->Get()->GetName().c_str());
      CleanFailedCommit();
      return err;
    }
  }

  return err;
}  // namespace android

auto DrmAtomicStateManager::ActivateDisplayUsingDPMS() -> int {
  return drmModeConnectorSetProperty(*pipe_->device->GetFd(),
                                     pipe_->connector->Get()->GetId(),
                                     pipe_->connector->Get()
                                         ->GetDpmsProperty()
                                         .GetId(),
                                     DRM_MODE_DPMS_ON);
}

}  // namespace android
