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

#include <pthread.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <thread>

#include <android-base/thread_annotations.h>

#include "compositor/LayerData.h"
#include "drm/DrmAtomicCommitSink.h"
#include "drm/DrmMode.h"
#include "drm/drm_mode.h"
#include "math/mat3.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

template <typename T>
class BindingOwner;

class IDrmFbIdHandle;
class DrmPlane;

struct DrmDisplayPipeline;

class DrmAtomicStateManager : public DrmAtomicCommitSink {
 public:
  static auto CreateInstance(DrmDisplayPipeline *pipe)
      -> std::unique_ptr<DrmAtomicStateManager>;

  ~DrmAtomicStateManager() override;

  bool TestAtomicCommit(AtomicCommitArgs &args) override;
  std::optional<AtomicCommitResult> ExecuteAtomicCommit(
      AtomicCommitArgs &args) override;
  bool IsActive() const override;
  void WaitLastFrame() override;

 private:
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
  struct AtomicRequest {
    AtomicRequest() = default;

    DrmModeAtomicReqUnique property_set;

    // Properties in the property set may reference the memory addresses of
    // these struct members.
    int wb_fence_address = -1;
    int out_fence_address = -1;

    KmsObjects used_kms_objects;
    KmsState new_frame_state;

    // Make this struct non-copyable and non-movable to avoid dangling
    // references to struct member addresses.
    AtomicRequest(const AtomicRequest &) = delete;
    AtomicRequest &operator=(const AtomicRequest &) = delete;
    AtomicRequest(AtomicRequest &&) = delete;
    AtomicRequest &operator=(AtomicRequest &&) = delete;
  };

  void StopThread();

  void ThreadFn();

  DrmAtomicStateManager() = default;
  std::optional<AtomicCommitResult> CommitFrame(AtomicCommitArgs &args,
                                                bool test_only);

  // Only accessed from main thread.
  DrmDisplayPipeline *pipe_{};

  // The following members must only be updated after a successful commit to
  // reflect the current state of DRM for the display.
  KmsState committed_frame_state_;
  DstRectInfo whole_display_rect_{};

  void CleanFailedCommit();
  bool SetWriteBackFenceIfNeeded(const AtomicCommitArgs &args,
                                 AtomicRequest &request);
  bool SetOutputFence(AtomicRequest &request);
  bool SetActiveIfNeeded(const AtomicCommitArgs &args, AtomicRequest &request);
  bool SetDisplayModeIfNeeded(const AtomicCommitArgs &args,
                              AtomicRequest &request);
  bool SetCtmIfNeeded(const AtomicCommitArgs &args, AtomicRequest &request);
  bool SetColorSpaceIfNeeded(const AtomicCommitArgs &args,
                             AtomicRequest &request);
  bool SetContentTypeIfNeeded(const AtomicCommitArgs &args,
                              AtomicRequest &request);
  bool SetContentProtectionIfNeeded(const AtomicCommitArgs &args,
                                    AtomicRequest &request);
  bool SetHdrMetadataIfNeeded(const AtomicCommitArgs &args,
                              AtomicRequest &request);
  bool SetMinBpcIfNeeded(const AtomicCommitArgs &args, AtomicRequest &request);
  bool SetCompositionIfNeeded(const AtomicCommitArgs &args,
                              AtomicRequest &request);

  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs &args);
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
  std::map<std::tuple<Colorspace, Colorspace>, const mat3> color_transform_map_;
};

}  // namespace android::drm_hwcomposer
