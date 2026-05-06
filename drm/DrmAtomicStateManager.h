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

#pragma once

#include <android-base/thread_annotations.h>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "compositor/LayerData.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmUnique.h"
#include "utils/ColorUtil.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

template <typename T>
class BindingOwner;

class IDrmFbIdHandle;
class DrmPlane;

class DrmAtomicStateManager : public AtomicStateManager {
 public:
  static auto CreateInstance(DrmDisplayPipeline *pipe)
      -> std::unique_ptr<DrmAtomicStateManager>;

  ~DrmAtomicStateManager() override;

  // Collection of kms objects that were committed to the kernel. There must be
  // a userspace handle to keep these from being removed/unregistered until the
  // commit that used them is no longer being presented.
  struct KmsObjects {
    /* We have to hold a reference to framebuffer while displaying it ,
     * otherwise picture will blink */
    std::vector<std::shared_ptr<IDrmFbIdHandle>> framebuffers;
    std::vector<DrmModeUserPropertyBlobUnique> blobs;
  };

  // State of the driver after a commit.
  struct KmsState {
    /* Required to cleanup unused planes */

    std::vector<std::shared_ptr<BindingOwner<DrmPlane>>> used_planes;

    /* To avoid setting the inactive state twice, which will fail the commit */
    bool crtc_active_state{};
  };

  // State for a pending atomic request. Includes the pending kms objects and
  // resulting state of the driver. Since the AtomicRequest might include
  // properties that reference memory addresses, this struct must not be moved
  // or copied.
  class DrmAtomicRequest : public AtomicRequest {
   public:
    DrmAtomicRequest() = default;
    ~DrmAtomicRequest() override = default;

    DrmModeAtomicReqUnique property_set;

    // Properties in the property set may reference the memory addresses of
    // these struct members.
    int wb_fence_address = -1;
    int out_fence_address = -1;

    KmsObjects used_kms_objects;
    KmsState new_frame_state;

    // Make this struct non-copyable and non-movable to avoid dangling
    // references to struct member addresses.
    DrmAtomicRequest(const DrmAtomicRequest &) = delete;
    DrmAtomicRequest &operator=(const DrmAtomicRequest &) = delete;
    DrmAtomicRequest(DrmAtomicRequest &&) = delete;
    DrmAtomicRequest &operator=(DrmAtomicRequest &&) = delete;
  };

  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs &args) override;
  AtomicCommitResult FinishPendingRequest(
      const AtomicCommitArgs &args, std::unique_ptr<AtomicRequest> request);
  bool IsActive() const override;
  void WaitLastFrame() override;

  auto GetDevice() const {
    return pipe_->device;
  }

 private:
  void StopThread();

  void ThreadFn();

  DrmAtomicStateManager() = default;

  // Only accessed from main thread.
  DrmDisplayPipeline *pipe_{};

  // The following members must only be updated after a successful commit to
  // reflect the current state of DRM for the display.
  KmsState committed_frame_state_;
  DstRectInfo whole_display_rect_{};

  bool SetWriteBackFenceIfNeeded(const AtomicCommitArgs &args,
                                 DrmAtomicRequest &request);
  bool SetOutputFence(DrmAtomicRequest &request);
  bool SetActiveIfNeeded(const AtomicCommitArgs &args,
                         DrmAtomicRequest &request);
  bool SetLinkStatusIfNeeded(DrmAtomicRequest &request);
  bool SetDisplayModeIfNeeded(const AtomicCommitArgs &args,
                              DrmAtomicRequest &request);
  bool SetCtmIfNeeded(const AtomicCommitArgs &args, DrmAtomicRequest &request);
  bool SetColorSpaceIfNeeded(const AtomicCommitArgs &args,
                             DrmAtomicRequest &request);
  bool SetContentTypeIfNeeded(const AtomicCommitArgs &args,
                              DrmAtomicRequest &request);
  bool SetContentProtectionIfNeeded(const AtomicCommitArgs &args,
                                    DrmAtomicRequest &request);
  bool SetHdrMetadataIfNeeded(const AtomicCommitArgs &args,
                              DrmAtomicRequest &request);
  bool SetMinBpcIfNeeded(const AtomicCommitArgs &args,
                         DrmAtomicRequest &request);
  bool SetCompositionIfNeeded(const AtomicCommitArgs &args,
                              DrmAtomicRequest &request);

  void CheckDoubleSettingState(AtomicCommitArgs &args) const;

  std::thread thread_;
  std::condition_variable cv_;
  std::mutex mutex_;

  // Accessed from both threads.
  void CleanupPriorFrameResources() REQUIRES(mutex_);

  bool exit_thread_ GUARDED_BY(mutex_){};
  // Front of the queue is the objects for the currently presented frame.
  // Objects for nonblocking frames are pushed to the back of the queue.
  std::queue<KmsObjects> frame_objects_ GUARDED_BY(mutex_);
  SharedFd last_present_fence_ GUARDED_BY(mutex_);
  int frames_staged_ GUARDED_BY(mutex_){};
  int frames_tracked_ GUARDED_BY(mutex_){};

  // Cached gamut mappings
  CscCache color_transform_map_;
  // Cached 1D LUTs
  Lut1DCache degamma_lut_1d_map_;
  Lut1DCache gamma_lut_1d_map_;
};

}  // namespace android::drm_hwcomposer
