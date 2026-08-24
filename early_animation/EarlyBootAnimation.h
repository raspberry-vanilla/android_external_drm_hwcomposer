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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bufferinfo/BufferInfo.h"
#include "drm/DrmFbImporter.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

class HwcDisplay;

class EarlyBootAnimation {
  friend class EarlyBootAnimationTest;

 public:
  explicit EarlyBootAnimation(HwcDisplay* display);
  ~EarlyBootAnimation();

  EarlyBootAnimation(const EarlyBootAnimation&) = delete;
  EarlyBootAnimation& operator=(const EarlyBootAnimation&) = delete;
  EarlyBootAnimation(EarlyBootAnimation&&) = delete;
  EarlyBootAnimation& operator=(EarlyBootAnimation&&) = delete;

  [[nodiscard]] bool Start();
  void Stop();
  void WaitForCompletion();

 private:
  enum class AnimationState {
    // Animation is completely inactive or terminated. The worker thread has
    // joined and all DRM framebuffers / dumb buffers have been freed or the
    // animation hasn't started yet.
    kStopped,

    // Animation playback is actively running in the background thread,
    // decoding and presenting frames to the display.
    kRunning,

    // Animation playback has finished all frames (or encountered an early
    // exit), but the final frame is still actively scanned out on screen.
    // DRM dumb buffers remain allocated until SurfaceFlinger presents its
    // first frame and invokes Stop(), preventing black screen flicker.
    kCompleted,
  };

  static constexpr size_t kAnimationHeaderSize = 32;

  struct AnimationHeader {
    std::array<char, 4> magic;  // "LZ4F" or similar
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t num_frames;
    uint32_t format;
    uint32_t hold_frame;
    uint32_t hold_duration_ms;
  } __attribute__((packed));
  static_assert(sizeof(AnimationHeader) == kAnimationHeaderSize,
                "AnimationHeader size must be exactly 32 bytes");

  [[nodiscard]] static uint32_t CalculateMultiplier(float vrefresh,
                                                    uint32_t anim_fps);
  [[nodiscard]] static std::chrono::nanoseconds CalculateFrameDuration(
      float vrefresh, uint32_t anim_fps);

  // Queries display configs to select the config with the highest refresh rate
  // matching the current resolution and color format (SDR vs HDR).
  // Maximizing the refresh rate aligns with SurfaceFlinger's default config
  // selection policy upon startup, preventing a disruptive modeset when
  // SurfaceFlinger takes over display control.
  [[nodiscard]] static ConfigId SelectBestConfig(
      const HwcDisplayConfig* current,
      const std::vector<HwcDisplayConfig>& configs, float anim_fps);

  // 240 Hz max refresh rate to prevent division-by-zero and bound frame timing.
  static constexpr uint32_t kMaxFps = 240;
  // 10,000 frames max (~2.7 min @ 60 FPS) to bound index table memory
  // allocation.
  static constexpr uint32_t kMaxFrames = 10000;
  // 10 second max hold duration as a safety watchdog against early boot hangs.
  static constexpr uint32_t kMaxHoldDurationMs = 10000;

  // Main animation worker: drives double-buffered playback, handles frame-rate
  // multiplier pacing, executes the post-animation hold, and transitions state_
  // to kCompleted.
  void Loop(UniqueFd fd);

  // Allocates double-buffered DRM dumb buffers, memory-maps them (mmap_addr),
  // imports DRM framebuffers (fb_id_handle), initializes scanout HwcLayers, and
  // calculates centering offsets (offset_x_, offset_y_).
  // Returns true on success
  [[nodiscard]] bool AllocateBuffers(uint32_t format,
                                     const HwcDisplayConfig* current_config);

  // Unmaps mmap_addr, closes prime_fd, destroys dumb buffers, and resets buffer
  // state.
  void FreeBuffers();

  // Reads and validates the 32-byte header into header_, determines compressed_
  // mode from magic, and initializes frame_data_offset_ to point past the
  // header. Returns true on success
  [[nodiscard]] bool ReadHeader(int fd);

  // Reads per-frame compressed sizes into frame_sizes_, precomputes cumulative
  // seek positions in frame_offsets_ for O(1) frame lookup, and advances
  // frame_data_offset_. Returns true on success
  [[nodiscard]] bool ReadIndexTable(int fd);
  void HandleFrameHold();
  [[nodiscard]] static bool ReadFrame(int fd, size_t frame_size,
                                      std::vector<uint8_t>& out_frame_buf);
  [[nodiscard]] bool ReadCompressedFrame(
      int fd, uint32_t frame_idx,
      std::vector<uint8_t>& out_compressed_buf) const;

  bool WriteFrameToDmaBuf(const std::vector<uint8_t>& frame_buf, int back_idx,
                          uint32_t bytes_per_pixel);
  [[nodiscard]] bool DecompressAndWriteFrame(
      const std::vector<uint8_t>& compressed_buf, uint32_t compressed_size,
      int back_idx, uint32_t bytes_per_pixel,
      std::vector<uint8_t>& out_decompressed_buf);
  [[nodiscard]] bool PresentFrame(int back_idx, uint32_t frame_count) const;
  void SetAnimationState(AnimationState state);

  // Non-owning pointer to the primary internal display. Owned by DrmHwc;
  // guaranteed valid for the animation lifetime because the animation is
  // destroyed prior to display deinit.
  HwcDisplay* const display_;

  std::thread thread_;
  std::atomic<AnimationState> state_{AnimationState::kStopped};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::once_flag stop_flag_;

  struct BootAnimBuffer {
    BufferInfo bi;
    std::shared_ptr<DrmFbIdHandle> fb_id_handle;
    void* mmap_addr = nullptr;
    size_t mmap_size = 0;
    uint32_t pitch = 0;
    int prime_fd = -1;
    std::unique_ptr<HwcLayer> hwc_layer;
  };

  static constexpr size_t kNumBuffers = 2;
  std::array<BootAnimBuffer, kNumBuffers> buffers_{};
  AnimationHeader header_{};
  std::vector<uint32_t> frame_sizes_;
  std::vector<uint64_t> frame_offsets_;
  uint64_t frame_data_offset_ = 0;
  bool compressed_ = false;
  std::chrono::nanoseconds frame_duration_{0};
  uint32_t offset_x_ = 0;
  uint32_t offset_y_ = 0;
  std::string animation_path_;
};

}  // namespace android::drm_hwcomposer
