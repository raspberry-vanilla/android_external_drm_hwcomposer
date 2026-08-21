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
#include <drm/drm_mode.h>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmUnique.h"
#include "utils/SlruCache.h"
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
    std::vector<std::shared_ptr<DrmModeUserPropertyBlobUnique>> cached_blobs;
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
  std::string DumpState() const override;

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
  bool ResetLinkStatus(DrmAtomicRequest &request);
  bool SetDisplayModeIfNeeded(const AtomicCommitArgs &args,
                              DrmAtomicRequest &request);
  bool SetCtmIfNeeded(const AtomicCommitArgs &args, DrmAtomicRequest &request);
  bool SetGammaIfNeeded(const AtomicCommitArgs &args,
                        DrmAtomicRequest &request);
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

  struct DegammaBlobKey {
    TransferFunction tf;
    size_t size;
    float scale;
    bool operator<(const DegammaBlobKey &o) const {
      return std::tie(tf, size, scale) < std::tie(o.tf, o.size, o.scale);
    }
  };

  struct GammaBlobKey {
    TransferFunction tf;
    size_t size;
    float scale;
    bool operator<(const GammaBlobKey &o) const {
      return std::tie(tf, size, scale) < std::tie(o.tf, o.size, o.scale);
    }
  };

  struct CtmBlobKey {
    enum class Kind { kCtm3x3, kCtm3x4, kOffset } kind;
    HwcColorspace src;
    HwcColorspace dest;
    HalColorTransformMatrix matrix;
    bool operator<(const CtmBlobKey &o) const {
      return std::tie(kind, src, dest, matrix) <
             std::tie(o.kind, o.src, o.dest, o.matrix);
    }
  };

  struct HdrMetadataBlobKey {
    hdr_output_metadata data;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
    // Compare semantic fields explicitly rather than using memcmp to avoid
    // strict weak ordering footguns caused by uninitialized struct padding
    // bytes.
    bool operator<(const HdrMetadataBlobKey &o) const {
      const auto &a = data.hdmi_metadata_type1;
      const auto &b = o.data.hdmi_metadata_type1;
      return std::tie(data.metadata_type, a.eotf, a.metadata_type,
                      a.display_primaries[0].x, a.display_primaries[0].y,
                      a.display_primaries[1].x, a.display_primaries[1].y,
                      a.display_primaries[2].x, a.display_primaries[2].y,
                      a.white_point.x, a.white_point.y,
                      a.max_display_mastering_luminance,
                      a.min_display_mastering_luminance, a.max_cll,
                      a.max_fall) <
             std::tie(o.data.metadata_type, b.eotf, b.metadata_type,
                      b.display_primaries[0].x, b.display_primaries[0].y,
                      b.display_primaries[1].x, b.display_primaries[1].y,
                      b.display_primaries[2].x, b.display_primaries[2].y,
                      b.white_point.x, b.white_point.y,
                      b.max_display_mastering_luminance,
                      b.min_display_mastering_luminance, b.max_cll, b.max_fall);
    }
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
  };

  using BlobKey = std::variant<DegammaBlobKey, GammaBlobKey, CtmBlobKey,
                               HdrMetadataBlobKey>;

  static constexpr size_t kMaxProbationaryBlobs = 16;
  static constexpr size_t kMaxProtectedBlobs = 48;

  // Segmented LRU cache for DRM property blobs (16 probationary, 48 protected)
  SlruCache<BlobKey, std::shared_ptr<DrmModeUserPropertyBlobUnique>,
            /*MaxProbationary=*/kMaxProbationaryBlobs,
            /*MaxProtected=*/kMaxProtectedBlobs>
      blob_cache_;

  // Looks up a property blob in the cache or lazily creates, registers, and
  // caches it via the provided generator callback on a cache miss.
  template <typename Key, typename Generator>
  auto GetOrCreateBlob(const Key &key, Generator &&generate_fn)
      -> std::shared_ptr<DrmModeUserPropertyBlobUnique>;

  // Cached color pipeline property
  // TODO: Remove after investigating resource manager initialization bug.
  bool use_color_pipeline_{};
};

}  // namespace android::drm_hwcomposer
