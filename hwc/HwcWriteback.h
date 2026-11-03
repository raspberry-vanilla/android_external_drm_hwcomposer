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

#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "drm/DrmConnector.h"
#include "drm/DrmDisplayPipeline.h"
#include "hwc/HwcLayer.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

using WritebackBuffer = HwcLayer::Buffer;

// Manages the resources, ownership, and state machine for a display writeback
// operation across virtual displays, one-shot physical readback, and non-DRM
// backends (such as SdmBackend).
//
// Connector Ownership (RAII via BindingOwner<DrmConnector>):
// - Virtual displays pass `pipeline_->connector`, sharing ownership so the
//   writeback connector remains bound to the pipeline for the entire lifetime
//   of the virtual display even as per-frame HwcWriteback instances are
//   replaced or destroyed.
// - One-shot physical writeback passes a newly bound writeback connector with
//   sole ownership; destroying HwcWriteback automatically unbinds the connector
//   when the BindingOwner reference count drops to zero.
// - Backends that do not use a DRM writeback connector (e.g. SdmBackend) pass
//   nullptr.
//
// Buffer and Fence State / Lifetime:
// 1. Staged (IsBufferConsumed() == false): Constructed in SetWritebackBuffer()
//    with a valid WritebackBuffer (BufferInfo, IDrmFbIdHandle, and in-fence).
//    If overwritten or destroyed before commit, RAII automatically closes the
//    in-fence and releases the buffer and framebuffer references.
// 2. Committed (IsBufferConsumed() == true): When the atomic commit completes,
//    the backend calls SetOutFence(), which clears `buffer_` (immediately
//    releasing the input buffer and closing the in-fence via RAII) and stores
//    the completion `out_fence_`. Passing an HwcWriteback whose buffer is
//    already consumed to a subsequent commit is rejected by the state manager.
// 3. Fence Taken / Destroyed: TakeWritebackBufferFence() extracts `out_fence_`
//    via TakeOutFence() and resets HwcDisplay's HwcWriteback instance,
//    releasing the one-shot connector binding. If TakeWritebackBufferFence() is
//    never called, `out_fence_` and `connector_` are cleaned up via RAII when
//    HwcWriteback is replaced or reset on the next frame or display teardown.
class HwcWriteback {
 public:
  HwcWriteback(std::shared_ptr<BindingOwner<DrmConnector>> connector,
               WritebackBuffer buffer)
      : connector_(std::move(connector)), buffer_(std::move(buffer)) {
  }

  ~HwcWriteback() = default;

  HwcWriteback(const HwcWriteback &) = delete;
  HwcWriteback &operator=(const HwcWriteback &) = delete;
  HwcWriteback(HwcWriteback &&) = delete;
  HwcWriteback &operator=(HwcWriteback &&) = delete;

  DrmConnector *GetConnector() const {
    return connector_ ? connector_->Get() : nullptr;
  }

  const std::optional<WritebackBuffer> &GetBuffer() const {
    return buffer_;
  }

  std::optional<WritebackBuffer> ConsumeBuffer() {
    auto buffer = std::move(buffer_);
    buffer_.reset();
    return buffer;
  }

  bool IsBufferConsumed() const {
    return !GetBuffer().has_value();
  }

  void SetOutFence(SharedFd fence) {
    buffer_.reset();
    out_fence_ = std::move(fence);
  }

  SharedFd TakeOutFence() {
    return std::move(out_fence_);
  }

 private:
  std::shared_ptr<BindingOwner<DrmConnector>> connector_;
  std::optional<WritebackBuffer> buffer_;
  SharedFd out_fence_;
};

}  // namespace android::drm_hwcomposer
