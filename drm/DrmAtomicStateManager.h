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

namespace android {

// Collection of kms objects that were committed to the kernel. There must be
// a userspace handle to keep these from being removed/unregistered until the
// commit that used them is no longer being presented.
struct KmsObjects {
  /* We have to hold a reference to framebuffer while displaying it ,
   * otherwise picture will blink */
  std::vector<std::shared_ptr<DrmFbIdHandle>> framebuffers;
  std::vector<DrmModeUserPropertyBlobUnique> blobs;
};

struct KmsState {
  /* Required to cleanup unused planes */
  std::vector<std::shared_ptr<BindingOwner<DrmPlane>>> used_planes;

  /* To avoid setting the inactive state twice, which will fail the commit */
  bool crtc_active_state{};
};

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
  KmsState new_frame_state;
  KmsObjects used_kms_objects;
  SharedFd out_writeback_complete_fence;
  SharedFd out_fence;
  // Shared FD can't be initiallized to an invalid value, for now we keep
  // the address separate from the FD for initialization.
  // TODO: look into adding support for invalid fences.
  int wb_fence_address = -1;
  int out_fence_address = -1;

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

  void CleanFailedCommit();

  void StopThread() {
    {
      const std::lock_guard lock(mutex_);
      exit_thread_ = true;
    }
    cv_.notify_all();
  }

 private:
  void ThreadFn();

  DrmAtomicStateManager() = default;
  bool CommitFrame(AtomicCommitArgs &args);

  // Only accessed from main thread.
  DrmDisplayPipeline *pipe_{};

  KmsState committed_frame_state_;

  KmsState staged_frame_state_;
  KmsObjects used_kms_objects_;

  void WaitLastFrame();
  bool SetWriteBackFenceIfNeeded(drmModeAtomicReq *pset,
                                 AtomicCommitArgs &args);
  bool SetOutputFence(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetActiveIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetDisplayModeIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetCtmIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetColorSpaceIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetContentTypeIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetHdrMetadataIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetMinBpcIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);
  bool SetCompositionIfNeeded(drmModeAtomicReq *pset, AtomicCommitArgs &args);

  DrmModeAtomicReqUnique GetAtomicModeReqForArgs(AtomicCommitArgs &args);
  static void CheckDoubleSettingState(AtomicCommitArgs &args,
                                      bool crtc_is_active);

  DstRectInfo whole_display_rect_{};

  std::thread thread_;
  std::condition_variable cv_;
  std::mutex mutex_;

  // Accessed from both threads.
  //
  void CleanupPriorFrameResources() REQUIRES(mutex_);

  bool exit_thread_ GUARDED_BY(mutex_){};
  // Front of the queue is the objects for the currently presented frame.
  // Objects for nonblocking frames are pushed to the back of the queue.
  std::queue<KmsObjects> frame_objects_ GUARDED_BY(mutex_);
  SharedFd last_present_fence_ GUARDED_BY(mutex_);
  int frames_staged_ GUARDED_BY(mutex_){};
  int frames_tracked_ GUARDED_BY(mutex_){};
};

}  // namespace android
