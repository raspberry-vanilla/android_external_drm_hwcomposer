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

#include "DrmAtomicStateManager.h"

#include <drm/drm_mode.h>
#include <sync/sync.h>
#include <utils/Trace.h>

#include <algorithm>
#include <cassert>

#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmPlane.h"
#include "drm/DrmUnique.h"
#include "drm/ResourceManager.h"
#include "utils/ColorUtil.h"
#include "utils/fd.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

auto DrmAtomicStateManager::CreateInstance(DrmDisplayPipeline *pipe)
    -> std::unique_ptr<DrmAtomicStateManager> {
  auto dasm = std::unique_ptr<DrmAtomicStateManager>(
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
  cl_args.composition = std::make_shared<LayerToPlaneJoiningPlan>();
  if (CommitFrame(cl_args, /* test_only */ false)) {
    ALOGE("Failed to clean-up active composition for pipeline %s",
          pipe_->connector->Get()->GetName().c_str());
  }
}

// NOLINTNEXTLINE (readability-function-cognitive-complexity): Fixme
std::optional<AtomicCommitResult> DrmAtomicStateManager::CommitFrame(
    AtomicCommitArgs &args, bool test_only) {
  // NOLINTNEXTLINE(misc-const-correctness)
  ATRACE_CALL();

  // Clear args.active if it's a no-op.
  CheckDoubleSettingState(args);

  if (!args.HasInputs()) {
    /* nothing to do */
    return AtomicCommitResult{};
  }

  if (!committed_frame_state_.crtc_active_state) {
    // Force args.active if the display is not active and there are other
    // things to commit.
    args.active = true;
  }

  auto atomic_request = GetAtomicModeReqForArgs(args);
  if (!atomic_request) {
    ALOGE("Failed to get property set");
    return std::nullopt;
  }

  uint32_t flags = args.seamless ? 0U : DRM_MODE_ATOMIC_ALLOW_MODESET;
  const int error_buf_max_size = 64;
  char err_buf[error_buf_max_size];
  auto *drm = pipe_->device;

  if (test_only) {
    ATRACE_NAME("TestOnlyCommit");
    auto err = drmModeAtomicCommit(*drm->GetFd(),
                                   atomic_request->property_set.get(),
                                   flags | DRM_MODE_ATOMIC_TEST_ONLY, drm);

    ALOGV_IF(err != 0, "Test-only seamless=%d ret=%d errno=%d strerror=%s\n",
             args.seamless, err, errno,
             strerror_r(errno, err_buf, error_buf_max_size));
    return err == 0 ? std::make_optional<AtomicCommitResult>() : std::nullopt;
  }

  WaitLastFrame();

  bool nonblock = !args.blocking && !args.active;

  flags |= nonblock ? DRM_MODE_ATOMIC_NONBLOCK : 0U;
  int err = 0;
  {
    ATRACE_NAME((nonblock ? "Commit_nonblock" : "Commit_block"));
    err = drmModeAtomicCommit(*drm->GetFd(), atomic_request->property_set.get(),
                              flags, drm);
  }

  if (err != 0 && args.seamless) {
    ALOGE(
        "Seamless commit failed, retrying a full modeset (visual artifacts may "
        "be observed). Error: %s",
        strerror_r(errno, err_buf, error_buf_max_size));

    ATRACE_NAME("SeamlessFallbackFullModesetCommit");

    err = drmModeAtomicCommit(*drm->GetFd(), atomic_request->property_set.get(),
                              flags | DRM_MODE_ATOMIC_ALLOW_MODESET, drm);
  }

  if (err != 0) {
    ALOGE("Failed to commit pset ret=%d errno=%d strerror=%s\n", err, errno,
          strerror_r(errno, err_buf, error_buf_max_size));
    return std::nullopt;
  }

  AtomicCommitResult result;
  result.present_fence = MakeSharedFd(atomic_request->out_fence_address);

  // Store the writeback fence if this operation used a writeback connector
  if (pipe_->writeback_connector && args.writeback_fb) {
    result.writeback_complete_fence = MakeSharedFd(
        atomic_request->wb_fence_address);
  }

  committed_frame_state_ = std::move(atomic_request->new_frame_state);

  if (args.display_mode) {
    auto raw_mode = args.display_mode.value().GetRawMode();
    whole_display_rect_.i_rect = {0, 0, raw_mode.hdisplay, raw_mode.vdisplay};
  }

  if (nonblock) {
    {
      const std::lock_guard lock(mutex_);
      last_present_fence_ = result.present_fence;
      frame_objects_.emplace(std::move(atomic_request->used_kms_objects));
      frames_staged_++;
    }
    cv_.notify_all();
  } else {
    const std::lock_guard lock(mutex_);
    last_present_fence_ = {};
    frame_objects_ = {};
    frame_objects_.emplace(std::move(atomic_request->used_kms_objects));
  }

  return result;
}

void DrmAtomicStateManager::CheckDoubleSettingState(
    AtomicCommitArgs &args) const {
  if (args.active && *args.active == committed_frame_state_.crtc_active_state) {
    /* Don't set the same state twice */
    args.active.reset();
  }
}

bool DrmAtomicStateManager::SetWriteBackFenceIfNeeded(
    const AtomicCommitArgs &args, AtomicRequest &request) {
  if (!pipe_->writeback_connector || !args.writeback_fb) {
    return true;
  }
  auto *crtc = pipe_->crtc->Get();

  if (!pipe_->writeback_connector->Get()
           ->GetCrtcIdProperty()
           .AtomicSet(*request.property_set, crtc->GetId())) {
    ALOGE("DrmAtomicStateManager: Failed to set writeback CRTC_ID property");
    return false;
  }

  if (!pipe_->writeback_connector->Get()
           ->GetWritebackFbIdProperty()
           .AtomicSet(*request.property_set, args.writeback_fb->GetFbId())) {
    ALOGE("DrmAtomicStateManager: Failed to set writeback FB_ID property");
    return false;
  }

  if (!pipe_->writeback_connector->Get()
           ->GetWritebackOutFenceProperty()
           .AtomicSet(*request.property_set,
                      uint64_t(&request.wb_fence_address))) {
    ALOGE(
        "DrmAtomicStateManager: Failed to set writeback OUT_FENCE_PTR "
        "property");
    return false;
  }

  // Wait on input fence if provided
  if (args.writeback_release_fence) {
    ATRACE_NAME("WritebackFenceWait");
    sync_wait(*args.writeback_release_fence, -1);
  }

  return true;
}

bool DrmAtomicStateManager::SetOutputFence(AtomicRequest &request) {
  auto *crtc = pipe_->crtc->Get();

  return crtc->GetOutFencePtrProperty()
      .AtomicSet(*request.property_set, uint64_t(&request.out_fence_address));
}

bool DrmAtomicStateManager::SetActiveIfNeeded(const AtomicCommitArgs &args,
                                              AtomicRequest &request) {
  if (!args.active) {
    return true;
  }
  auto *crtc = pipe_->crtc->Get();
  auto *connector = pipe_->connector->Get();
  request.new_frame_state.crtc_active_state = *args.active;
  if (!crtc->GetActiveProperty().AtomicSet(*request.property_set,
                                           *args.active ? 1 : 0) ||
      !connector->GetCrtcIdProperty().AtomicSet(*request.property_set,
                                                crtc->GetId())) {
    return false;
  }
  if (!*args.active && args.teardown) {
    if (!connector->GetCrtcIdProperty().AtomicSet(*request.property_set, 0) ||
        !crtc->GetModeProperty().AtomicSet(*request.property_set, 0)) {
      return false;
    }
  }

  return true;
}

bool DrmAtomicStateManager::SetDisplayModeIfNeeded(const AtomicCommitArgs &args,
                                                   AtomicRequest &request) {
  if (!args.display_mode) {
    return true;
  }

  auto *drm = pipe_->device;
  auto mode_blob = args.display_mode.value().CreateModeBlob(*drm);

  if (!mode_blob) {
    ALOGE("Failed to create mode_blob");
    return false;
  }

  auto *crtc = pipe_->crtc->Get();
  if (!crtc->GetModeProperty().AtomicSet(*request.property_set, *mode_blob)) {
    return false;
  }
  request.used_kms_objects.blobs.emplace_back(std::move(mode_blob));
  return true;
}

bool DrmAtomicStateManager::SetCtmIfNeeded(const AtomicCommitArgs &args,
                                           AtomicRequest &request) {
  auto *crtc = pipe_->crtc->Get();
  if (!args.color_matrix || !crtc->GetCtmProperty()) {
    return true;
  }

  auto *drm = pipe_->device;
  if (drm->GetResMan().UseColorPipeline()) {
    return crtc->GetCtmProperty().AtomicSet(*request.property_set, 0);
  }
  auto drm_color_matrix = ColorUtil::ToColorTransform3x3(args.color_matrix);
  auto ctm_blob = drm->RegisterUserPropertyBlob(drm_color_matrix.get(),
                                                sizeof(drm_color_ctm));
  if (!ctm_blob) {
    ALOGE("Failed to create CTM blob");
    return false;
  }

  if (!crtc->GetCtmProperty().AtomicSet(*request.property_set, *ctm_blob)) {
    return false;
  }

  request.used_kms_objects.blobs.emplace_back(std::move(ctm_blob));
  return true;
}

bool DrmAtomicStateManager::SetColorSpaceIfNeeded(const AtomicCommitArgs &args,
                                                  AtomicRequest &request) {
  auto *connector = pipe_->connector->Get();
  if (!args.colorspace || !connector->GetColorspaceProperty()) {
    return true;
  }

  return connector->GetColorspaceProperty()
      .AtomicSet(*request.property_set,
                 connector->GetColorspacePropertyValue(*args.colorspace));
}

bool DrmAtomicStateManager::SetContentTypeIfNeeded(const AtomicCommitArgs &args,
                                                   AtomicRequest &request) {
  auto *connector = pipe_->connector->Get();
  if (!args.content_type || !connector->GetContentTypeProperty()) {
    return true;
  }
  return connector->GetContentTypeProperty().AtomicSet(*request.property_set,
                                                       static_cast<uint64_t>(
                                                           *args.content_type));
}

bool DrmAtomicStateManager::SetContentProtectionIfNeeded(
    const AtomicCommitArgs &args, AtomicRequest &request) {
  auto *connector = pipe_->connector->Get();
  if (!args.content_protection.has_value() ||
      !args.hdcp_content_type.has_value() ||
      !connector->GetContentProtectionProperty() ||
      !connector->GetHdcpContentTypeProperty()) {
    return true;
  }
  if (!connector->GetContentProtectionProperty()
           .AtomicSet(*request.property_set,
                      static_cast<uint64_t>(args.content_protection.value()))) {
    return false;
  }
  return connector->GetHdcpContentTypeProperty()
      .AtomicSet(*request.property_set,
                 static_cast<uint64_t>(args.hdcp_content_type.value()));
}

bool DrmAtomicStateManager::SetHdrMetadataIfNeeded(const AtomicCommitArgs &args,
                                                   AtomicRequest &request) {
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
           .AtomicSet(*request.property_set, *hdr_metadata_blob)) {
    return false;
  }
  request.used_kms_objects.blobs.emplace_back(std::move(hdr_metadata_blob));

  return true;
}

bool DrmAtomicStateManager::SetMinBpcIfNeeded(const AtomicCommitArgs &args,
                                              AtomicRequest &request) {
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
  return connector->GetMinBpcProperty().AtomicSet(*request.property_set,
                                                  min_bpc_val);
}

bool DrmAtomicStateManager::SetCompositionIfNeeded(const AtomicCommitArgs &args,
                                                   AtomicRequest &request) {
  if (!args.composition) {
    return true;
  }

  // Initialize the list of unused planes to all the planes used in the
  // previous frame.
  auto unused_planes = committed_frame_state_.used_planes;
  // Clear the list of planes for the next frame. It will be repopulated below.
  request.new_frame_state.used_planes.clear();

  for (auto &joining : args.composition->plan) {
    DrmPlane *plane = joining.plane->Get();
    LayerData &layer = joining.layer;

    request.used_kms_objects.framebuffers.emplace_back(layer.fb);
    request.new_frame_state.used_planes.emplace_back(joining.plane);

    /* Remove from 'unused' list, since plane is re-used */
    auto &v = unused_planes;
    v.erase(std::remove(v.begin(), v.end(), joining.plane), v.end());

    DrmModeUserPropertyBlobUnique damage_blob;
    auto *crtc = pipe_->crtc->Get();

    DstRectInfo display_rect_info = whole_display_rect_;
    if (args.display_mode) {
      auto raw_mode = args.display_mode.value().GetRawMode();
      display_rect_info.i_rect = {0, 0, raw_mode.hdisplay, raw_mode.vdisplay};
    }

    if (plane->AtomicSetState(*request.property_set, layer, joining.z_pos,
                              crtc->GetId(), display_rect_info,
                              damage_blob) != 0) {
      return false;
    }
    request.used_kms_objects.blobs.emplace_back(std::move(damage_blob));

    if (pipe_->device->GetResMan().UseColorPipeline()) {
      std::shared_ptr<drm_color_ctm_3x4> drm_color_matrix = ColorUtil::
          GamutAdjustIfNeeded(layer.colorspace,
                              args.colorspace.value_or(Colorspace::kDefault),
                              args.color_matrix, color_transform_map_);
      DrmModeUserPropertyBlobUnique ctm_3x4_blob;
      if (drm_color_matrix) {
        ctm_3x4_blob = pipe_->device
                           ->RegisterUserPropertyBlob(drm_color_matrix.get(),
                                                      sizeof(
                                                          drm_color_ctm_3x4));
      }
      if (plane->AtomicSetColorPipeline(*request.property_set, ctm_3x4_blob) !=
          0) {
        return false;
      }
      request.used_kms_objects.blobs.emplace_back(std::move(ctm_3x4_blob));
    }
  }

  // Disable all planes that were used in the previous commit which are no
  // longer being used.
  return std::all_of(unused_planes.begin(), unused_planes.end(),
                     [&request](auto &plane) {
                       return plane->Get()->AtomicDisablePlane(
                                  *request.property_set) == 0;
                     });
}

std::unique_ptr<DrmAtomicStateManager::AtomicRequest>
DrmAtomicStateManager::GetAtomicModeReqForArgs(AtomicCommitArgs &args) {
  ATRACE_CALL();
  auto atomic_request = std::make_unique<AtomicRequest>();
  atomic_request->property_set = MakeDrmModeAtomicReqUnique();
  if (!atomic_request->property_set) {
    ALOGE("Failed to allocate property set");
    return nullptr;
  }

  // Initialize to the current state, and update as needed with the below
  // helpers.
  atomic_request->new_frame_state = committed_frame_state_;

  if (!SetWriteBackFenceIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set writeback fence");
    return nullptr;
  }

  if (!SetOutputFence(*atomic_request)) {
    ALOGE("Failed to set output fence");
    return nullptr;
  }

  if (!SetActiveIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set active");
    return nullptr;
  }

  if (!SetDisplayModeIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set display mode");
    return nullptr;
  }

  if (!SetCtmIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set CTM blob");
    return nullptr;
  }

  if (!SetColorSpaceIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set color space");
    return nullptr;
  }

  if (!SetContentTypeIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set content type");
    return nullptr;
  }

  if (!SetContentProtectionIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set Content Protection and HDCP Content Type");
    return nullptr;
  }

  if (!SetHdrMetadataIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set HDR metadata");
    return nullptr;
  }

  if (!SetMinBpcIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set min BPC");
    return nullptr;
  }
  if (!SetCompositionIfNeeded(args, *atomic_request)) {
    ALOGE("Failed to set composition");
    return nullptr;
  }

  return atomic_request;
}

void DrmAtomicStateManager::StopThread() {
  {
    const std::lock_guard lock(mutex_);
    exit_thread_ = true;
  }
  cv_.notify_all();
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

bool DrmAtomicStateManager::TestAtomicCommit(AtomicCommitArgs &args) {
  auto result = CommitFrame(args, /* test_only */ true);
  return result.has_value();
}

std::optional<AtomicCommitResult> DrmAtomicStateManager::ExecuteAtomicCommit(
    AtomicCommitArgs &args) {
  auto result = CommitFrame(args, /* test_only */ false);
  if (result) {
    return result;
  }

  ALOGE("Composite failed for pipeline %s",
        pipe_->connector->Get()->GetName().c_str());
  CleanFailedCommit();
  return std::nullopt;
}

bool DrmAtomicStateManager::IsActive() const {
  return committed_frame_state_.crtc_active_state;
}

}  // namespace android::drm_hwcomposer
