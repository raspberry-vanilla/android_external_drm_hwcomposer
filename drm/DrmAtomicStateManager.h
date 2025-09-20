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

#include <memory>
#include <optional>
#include <queue>

#include "compositor/DisplayInfo.h"
#include "compositor/DrmKmsPlan.h"
#include "compositor/LayerData.h"
#include "drm/DrmPlane.h"
#include "drm/ResourceManager.h"
#include "drm/VSyncWorker.h"

namespace android::drm_hwcomposer {

struct AtomicCommitArgs {
  /* inputs. All fields are optional, but at least one has to be specified */
  bool test_only = false;
  bool blocking = false;
  bool teardown = false;
  bool seamless = false;
  std::optional<DrmMode> display_mode;
  std::optional<bool> active;
  std::shared_ptr<DrmKmsPlan> composition;
  std::shared_ptr<drm_color_ctm> color_matrix;
  std::optional<Colorspace> colorspace;
  std::optional<ContentType> content_type;
  std::shared_ptr<hdr_output_metadata> hdr_metadata;
  std::optional<int32_t> min_bpc;

  std::shared_ptr<DrmFbIdHandle> writeback_fb;
  SharedFd writeback_release_fence;

  /* out */
  SharedFd out_writeback_complete_fence;
  SharedFd out_fence;

  /* helpers */
  auto HasInputs() const -> bool {
    return display_mode || active || composition;
  }
};

class DrmAtomicStateManager {
 public:
  static auto CreateInstance(DrmDisplayPipeline *pipe)
      -> std::shared_ptr<DrmAtomicStateManager>;

  ~DrmAtomicStateManager();

  bool ExecuteAtomicCommit(AtomicCommitArgs &args);
  auto ActivateDisplayUsingDPMS() -> int;

  void StopThread() {
    {
      const std::lock_guard lock(mutex_);
      exit_thread_ = true;
    }
    cv_.notify_all();
  }

  void WaitLastFrame();

 private:
  // Collection of kms objects that were committed to the kernel. There must be
  // a userspace handle to keep these from being removed/unregistered until the
  // commit that used them is no longer being presented.
  struct KmsObjects {
    /* We have to hold a reference to framebuffer while displaying it ,
     * otherwise picture will blink */
    std::vector<std::shared_ptr<DrmFbIdHandle>> framebuffers;
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

  void ThreadFn();

  DrmAtomicStateManager() = default;
  bool CommitFrame(AtomicCommitArgs &args);

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
};

}  // namespace android::drm_hwcomposer
