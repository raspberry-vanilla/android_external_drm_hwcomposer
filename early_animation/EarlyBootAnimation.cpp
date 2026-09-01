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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "EarlyBootAnimation.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <cutils/trace.h>  // IWYU pragma: keep
#include <drm_fourcc.h>
#include <fcntl.h>  // IWYU pragma: keep
#include <linux/dma-buf.h>
#if HAS_LZ4
#include <lz4.h>
#endif
#include <sys/ioctl.h>  // IWYU pragma: keep
#include <sys/mman.h>   // IWYU pragma: keep
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Trace.h>  // IWYU pragma: keep
#include <xf86drm.h>      // IWYU pragma: keep

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/CommitStatus.h"
#include "drm/DrmDevice.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/fd.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {

constexpr double kNanosecondsPerSecond = 1e9;

constexpr bool IsHdrFormat(uint32_t format) {
  switch (format) {
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_RGBX1010102:
      return true;
    default:
      return false;
  }
}

}  // namespace

EarlyBootAnimation::EarlyBootAnimation(HwcDisplay* display)
    : display_(display), animation_path_(Properties::BootAnimationPath()) {
  Properties::SetBootAnimationCompleted(false);
}

EarlyBootAnimation::~EarlyBootAnimation() {
  Stop();
}

uint32_t EarlyBootAnimation::CalculateMultiplier(float vrefresh,
                                                 uint32_t anim_fps) {
  if (vrefresh > static_cast<float>(anim_fps) && anim_fps > 0) {
    return std::max(1U, static_cast<uint32_t>(std::round(
                            vrefresh / static_cast<float>(anim_fps))));
  }
  return 1U;
}

std::chrono::nanoseconds EarlyBootAnimation::CalculateFrameDuration(
    float vrefresh, uint32_t anim_fps) {
  uint32_t multiplier = CalculateMultiplier(vrefresh, anim_fps);
  if (vrefresh > 0.0F) {
    return std::chrono::nanoseconds(static_cast<uint64_t>(
        std::round((kNanosecondsPerSecond * static_cast<double>(multiplier)) /
                   static_cast<double>(vrefresh))));
  }
  return std::chrono::nanoseconds(static_cast<uint64_t>(kNanosecondsPerSecond) /
                                  std::max(1U, anim_fps));
}

bool EarlyBootAnimation::Start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != AnimationState::kStopped || thread_.joinable()) {
      ALOGE(
          "EarlyBootAnimation: Attempting to start while animation is running");
      return false;
    }
  }

  UniqueFd fd = MakeUniqueFd(
      // NOLINTNEXTLINE(misc-include-cleaner)
      open(animation_path_.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd) {
    ALOGE("EarlyBootAnimation: Failed to open %s (errno=%d)",
          animation_path_.c_str(), errno);
    Stop();
    return false;
  }

  if (!ReadHeader(*fd)) {
    Stop();
    return false;
  }

  if (compressed_) {
    if (!ReadIndexTable(*fd)) {
      Stop();
      return false;
    }
  }

  const HwcDisplayConfig* current_config = display_->GetCurrentConfig();
  if (current_config == nullptr) {
    ALOGE("EarlyBootAnimation: No current display config");
    Stop();
    return false;
  }
  ConfigId best_config_id = SelectBestConfig(current_config,
                                             display_->GetDisplayConfigs(),
                                             static_cast<float>(header_.fps));

  ALOGD("EarlyBootAnimation: Current config is %u (%ux%u @ %.2f Hz)",
        current_config->id, current_config->mode.GetRawMode().hdisplay,
        current_config->mode.GetRawMode().vdisplay,
        current_config->mode.GetVRefresh());
  if (best_config_id == current_config->id) {
    ALOGD("EarlyBootAnimation: Keeping current config %u to match FPS %u",
          current_config->id, header_.fps);
  } else {
    ALOGD("EarlyBootAnimation: Switching config from %u to %u to match FPS %u",
          current_config->id, best_config_id, header_.fps);
    display_->SetConfig(best_config_id);
    current_config = display_->GetCurrentConfig();
    if (current_config == nullptr) {
      ALOGE("EarlyBootAnimation: No current display config after SetConfig");
      Stop();
      return false;
    }
  }

  // Match sleep duration to the panel's actual refresh rate to avoid clock
  // drift (e.g. 59.94Hz), and use multiplier for high refresh panels (e.g.
  // 120Hz).
  float vrefresh = current_config != nullptr
                       ? current_config->mode.GetVRefresh()
                       : static_cast<float>(header_.fps);
  frame_duration_ = CalculateFrameDuration(vrefresh, header_.fps);

  if (!AllocateBuffers(header_.format, current_config)) {
    ALOGE("EarlyBootAnimation: Failed to allocate buffers");
    Stop();
    return false;
  }

  float effective_fps = (frame_duration_.count() > 0)
                            ? static_cast<float>(
                                  kNanosecondsPerSecond /
                                  static_cast<double>(frame_duration_.count()))
                            : static_cast<float>(header_.fps);
  ALOGD(
      "EarlyBootAnimation: Starting: %ux%u @ %.2f FPS (panel %.2f Hz, anim %u "
      "FPS), %u frames, format 0x%08x",
      header_.width, header_.height, effective_fps, vrefresh, header_.fps,
      header_.num_frames, static_cast<uint32_t>(header_.format));

  SetAnimationState(AnimationState::kRunning);
  thread_ = std::thread(&EarlyBootAnimation::Loop, this, std::move(fd));
  return true;
}

bool EarlyBootAnimation::ReadIndexTable(int fd) {
  const size_t index_table_offset = sizeof(AnimationHeader);
  size_t table_size = header_.num_frames * sizeof(uint32_t);
  frame_data_offset_ = index_table_offset + table_size;

  if (lseek(fd, static_cast<off_t>(index_table_offset), SEEK_SET) ==
      static_cast<off_t>(-1)) {
    ALOGE("EarlyBootAnimation: Failed to seek to index table");
    return false;
  }

  frame_sizes_.resize(header_.num_frames);
  if (read(fd, frame_sizes_.data(), table_size) !=
      static_cast<ssize_t>(table_size)) {
    ALOGE("EarlyBootAnimation: Failed to read index table data");
    frame_sizes_.clear();
    return false;
  }

  static constexpr uint32_t kBitsPerByte = 8U;
  uint32_t bytes_per_pixel = BufferInfoGetter::DrmFormatToBpp(header_.format) /
                             kBitsPerByte;
  if (bytes_per_pixel == 0) {
    frame_sizes_.clear();
    return false;
  }
  size_t max_uncompressed = static_cast<size_t>(header_.width) *
                            header_.height * bytes_per_pixel;
#if HAS_LZ4
  size_t max_allowed_compressed = LZ4_compressBound(
      static_cast<int>(max_uncompressed));
#else
  size_t max_allowed_compressed = max_uncompressed;
#endif

  // Precompute cumulative frame offsets for O(1) seeking during playback.
  frame_offsets_.resize(header_.num_frames);
  uint64_t current_offset = frame_data_offset_;
  for (uint32_t i = 0; i < header_.num_frames; ++i) {
    if (frame_sizes_[i] == 0 || frame_sizes_[i] > max_allowed_compressed) {
      ALOGE("EarlyBootAnimation: Invalid compressed frame size %u at index %u",
            frame_sizes_[i], i);
      frame_sizes_.clear();
      frame_offsets_.clear();
      return false;
    }
    if (std::numeric_limits<uint64_t>::max() - current_offset <
        frame_sizes_[i]) {
      ALOGE("EarlyBootAnimation: Frame offset overflow at index %u", i);
      frame_sizes_.clear();
      frame_offsets_.clear();
      return false;
    }
    frame_offsets_[i] = current_offset;
    current_offset += frame_sizes_[i];
  }

  struct stat st{};
  if (fstat(fd, &st) == -1) {
    ALOGE("EarlyBootAnimation: Failed to stat animation file");
    frame_sizes_.clear();
    frame_offsets_.clear();
    return false;
  }

  if (current_offset > static_cast<uint64_t>(st.st_size)) {
    ALOGE(
        "EarlyBootAnimation: Total frame data size %llu exceeds file size %lld",
        static_cast<unsigned long long>(current_offset),
        static_cast<long long>(st.st_size));
    frame_sizes_.clear();
    frame_offsets_.clear();
    return false;
  }

  return true;
}

void EarlyBootAnimation::SetAnimationState(AnimationState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == state) {
    return;
  }
  state_ = state;
  cv_.notify_all();
}

void EarlyBootAnimation::WaitForCompletion() {
  ATRACE_CALL();
  std::unique_lock<std::mutex> lock(mutex_);
  // Short-circuit if already completed or stopped.
  if (state_ == AnimationState::kCompleted ||
      (state_ == AnimationState::kStopped && !thread_.joinable())) {
    return;
  }

  // Calculate the total expected animation runtime dynamically: total frame
  // playback duration (num_frames * frame_duration_) plus the first-frame hold
  // duration plus the programmable post-frame hold duration (hold_duration_ms),
  // with an added 3-second safety watchdog buffer to prevent false timeouts.
  const auto expected_duration = (header_.num_frames * frame_duration_) +
                                 std::chrono::milliseconds(
                                     Properties::EarlyBootHoldMs()) +
                                 std::chrono::milliseconds(
                                     header_.hold_duration_ms);
  static constexpr auto kWatchdogBuffer = std::chrono::seconds(3);
  const auto max_wait_duration = expected_duration + kWatchdogBuffer;

  ALOGD(
      "EarlyBootAnimation: Waiting for animation to complete (timeout: %" PRId64
      " ms)",
      static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              max_wait_duration)
              .count()));

  if (!cv_.wait_for(lock, max_wait_duration, [this] {
        return state_ == AnimationState::kCompleted ||
               state_ == AnimationState::kStopped;
      })) {
    ALOGW(
        "EarlyBootAnimation: Timed out waiting for animation to complete after "
        "%" PRId64 " ms",
        static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                max_wait_duration)
                .count()));
  }
}

void EarlyBootAnimation::Stop() {
  ATRACE_CALL();
  SetAnimationState(AnimationState::kStopped);

  // Ensure teardown runs exactly once across concurrent callers (e.g.
  // boot_thread_ and SurfaceFlinger) without holding mutex_ while joining.
  std::call_once(stop_flag_, [this]() {
    ALOGD("EarlyBootAnimation: Stopping animation");
    if (thread_.joinable()) {
      ALOGD("EarlyBootAnimation: Joining thread");
      thread_.join();
    }

    FreeBuffers();
    Properties::SetBootAnimationCompleted(true);
    ALOGD("EarlyBootAnimation: Stopped");
  });
}

void EarlyBootAnimation::Loop(UniqueFd fd) {
  ATRACE_CALL();

  const HwcDisplayConfig* active_config = display_->GetCurrentConfig();
  if (active_config == nullptr) {
    ALOGE("EarlyBootAnimation: No active config");
    SetAnimationState(AnimationState::kCompleted);
    return;
  }

  if (!fd) {
    ALOGE("EarlyBootAnimation: Invalid animation file descriptor in loop");
    SetAnimationState(AnimationState::kCompleted);
    return;
  }

  if (lseek(*fd, static_cast<off_t>(frame_data_offset_), SEEK_SET) ==
      static_cast<off_t>(-1)) {
    ALOGE("EarlyBootAnimation: Failed to seek to frame data offset %llu",
          static_cast<unsigned long long>(frame_data_offset_));
    SetAnimationState(AnimationState::kCompleted);
    return;
  }
  uint32_t drm_format = header_.format;
  static constexpr uint32_t kBitsPerByte = 8U;
  uint32_t bytes_per_pixel = BufferInfoGetter::DrmFormatToBpp(drm_format) /
                             kBitsPerByte;

  // Pre-allocate buffers
  const uint32_t
      max_compressed_size = (compressed_ && !frame_sizes_.empty())
                                ? *std::max_element(frame_sizes_.begin(),
                                                    frame_sizes_.end())
                                : 0U;
  std::vector<uint8_t> compressed_buf(max_compressed_size);

  size_t frame_size = static_cast<size_t>(header_.width) * header_.height *
                      bytes_per_pixel;
  std::vector<uint8_t> frame_buf(frame_size);

  int back_idx = 0;
  uint32_t frame_count = 0;
  uint32_t hold_count = 0;

  while (state_ == AnimationState::kRunning) {
    ATRACE_NAME("HwcBootAnimFrame");
    ATRACE_INT("HwcBootAnimFrameIdx", frame_count);
    auto frame_start = std::chrono::steady_clock::now();

    if (frame_count >= header_.num_frames) {
      ALOGD("EarlyBootAnimation: Reached end of frames, stopping animation");
      SetAnimationState(AnimationState::kCompleted);
      break;
    }

    if (compressed_) {
      if (!ReadCompressedFrame(*fd, frame_count, compressed_buf) ||
          !DecompressAndWriteFrame(compressed_buf, frame_sizes_[frame_count],
                                   back_idx, bytes_per_pixel, frame_buf)) {
        ALOGE("EarlyBootAnimation: Failed reading compressed frame %u",
              frame_count);
        SetAnimationState(AnimationState::kCompleted);
        break;
      }
    } else {
      if (!ReadFrame(*fd, frame_size, frame_buf) ||
          !WriteFrameToDmaBuf(frame_buf, back_idx, bytes_per_pixel)) {
        ALOGE("EarlyBootAnimation: Failed to write raw frame %u to dma buffer",
              frame_count);
        SetAnimationState(AnimationState::kCompleted);
        break;
      }
    }

    if (!PresentFrame(back_idx, frame_count)) {
      ALOGE(
          "EarlyBootAnimation: Failed to present frame %u, stopping animation",
          frame_count);
      SetAnimationState(AnimationState::kCompleted);
      break;
    }

    if (frame_count == 0) {
      const auto first_frame_hold = std::chrono::milliseconds(
          Properties::EarlyBootHoldMs());
      ALOGD("EarlyBootAnimation: First frame committed, holding for %lldms",
            static_cast<long long>(first_frame_hold.count()));
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, first_frame_hold, [this] {
              return state_ != AnimationState::kRunning;
            })) {
          ALOGD(
              "EarlyBootAnimation: Cancelled first-frame hold due to inactive "
              "animation");
          break;
        }
      }
    }

    // Programmable frame hold logic: hold the target frame once committed to
    // display
    if (frame_count == header_.hold_frame && hold_count == 0) {
      HandleFrameHold();
      hold_count = 1;
    }

    back_idx = 1 - back_idx;
    frame_count++;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_until(lock, frame_start + frame_duration_,
                     [this] { return state_ != AnimationState::kRunning; });
    }
  }

  if (state_ == AnimationState::kRunning) {
    SetAnimationState(AnimationState::kCompleted);
  }

  ALOGD("EarlyBootAnimation: Loop finished. Processed %u frames", frame_count);
}

bool EarlyBootAnimation::ReadHeader(int fd) {
  if (lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) {
    ALOGE("EarlyBootAnimation: Failed to seek to start of header");
    return false;
  }
  if (read(fd, &header_, sizeof(header_)) != sizeof(header_)) {
    ALOGE("EarlyBootAnimation: Failed to read header");
    return false;
  }
  if (memcmp(header_.magic.data(), "RAWF", 4) == 0) {
    compressed_ = false;
  } else if (memcmp(header_.magic.data(), "LZ4F", 4) == 0) {
#if HAS_LZ4
    compressed_ = true;
#else
    ALOGE(
        "EarlyBootAnimation: LZ4 compressed animation not supported (built "
        "without HAS_LZ4)");
    return false;
#endif
  } else {
    ALOGE(
        "EarlyBootAnimation: Invalid magic 0x%02x%02x%02x%02x (expected RAWF "
        "or LZ4F)",
        static_cast<uint8_t>(header_.magic[0]),
        static_cast<uint8_t>(header_.magic[1]),
        static_cast<uint8_t>(header_.magic[2]),
        static_cast<uint8_t>(header_.magic[3]));
    return false;
  }

  if (header_.width == 0 || header_.height == 0) {
    ALOGE("EarlyBootAnimation: Invalid dimensions %ux%u", header_.width,
          header_.height);
    return false;
  }
  if (header_.fps == 0 || header_.fps > kMaxFps) {
    ALOGE("EarlyBootAnimation: Invalid FPS %u", header_.fps);
    return false;
  }
  if (header_.num_frames == 0 || header_.num_frames > kMaxFrames) {
    ALOGE("EarlyBootAnimation: Invalid frame count %u", header_.num_frames);
    return false;
  }

  if (header_.hold_duration_ms > kMaxHoldDurationMs) {
    ALOGE("EarlyBootAnimation: Hold duration %u ms exceeds %u ms",
          header_.hold_duration_ms, kMaxHoldDurationMs);
    return false;
  }
  if (header_.hold_frame >= header_.num_frames &&
      header_.hold_duration_ms > 0) {
    ALOGW("EarlyBootAnimation: hold_frame %u >= num_frames %u",
          header_.hold_frame, header_.num_frames);
    return false;
  }

  frame_data_offset_ = sizeof(AnimationHeader);
  return true;
}

void EarlyBootAnimation::HandleFrameHold() {
  ALOGD("EarlyBootAnimation: Reached frame %u, starting %ums hold",
        header_.hold_frame, header_.hold_duration_ms);
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cv_.wait_for(lock, std::chrono::milliseconds(header_.hold_duration_ms),
                     [this] { return state_ != AnimationState::kRunning; })) {
      ALOGD("EarlyBootAnimation: Cancelled hold due to inactive animation");
      return;
    }
  }
  ALOGD("EarlyBootAnimation: Completed frame hold");
}

bool EarlyBootAnimation::ReadFrame(int fd, size_t frame_size,
                                   std::vector<uint8_t>& out_frame_buf) {
  // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
  ssize_t bytes_read = read(fd, out_frame_buf.data(), frame_size);
  if (bytes_read != static_cast<ssize_t>(frame_size)) {
    ALOGD("EarlyBootAnimation: Reached end of file");
    return false;
  }
  return true;
}

bool EarlyBootAnimation::ReadCompressedFrame(
    int fd, uint32_t frame_idx,
    std::vector<uint8_t>& out_compressed_buf) const {
  if (frame_idx >= header_.num_frames || frame_idx >= frame_offsets_.size()) {
    ALOGE("Invalid frame ID: %u", frame_idx);
    return false;
  }

  uint64_t offset = frame_offsets_[frame_idx];
  if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) ==
      static_cast<off_t>(-1)) {
    ALOGE("EarlyBootAnimation: Failed to seek to frame %u (offset=%llu)",
          frame_idx, static_cast<unsigned long long>(offset));
    return false;
  }

  uint32_t compressed_size = frame_sizes_[frame_idx];
  if (compressed_size > out_compressed_buf.size()) {
    ALOGE("EarlyBootAnimation: Compressed size %u exceeds buffer size %zu",
          compressed_size, out_compressed_buf.size());
    return false;
  }
  // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
  if (read(fd, out_compressed_buf.data(), compressed_size) !=
      static_cast<ssize_t>(compressed_size)) {
    ALOGE("EarlyBootAnimation: Failed to read compressed frame %u", frame_idx);
    return false;
  }

  return true;
}

#if HAS_LZ4
bool EarlyBootAnimation::DecompressAndWriteFrame(
    const std::vector<uint8_t>& compressed_buf, uint32_t compressed_size,
    int back_idx, uint32_t bytes_per_pixel,
    std::vector<uint8_t>& out_decompressed_buf) {
  const int decompressed_bytes = LZ4_decompress_safe(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const char*>(compressed_buf.data()),
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<char*>(out_decompressed_buf.data()),
      static_cast<int>(compressed_size),
      static_cast<int>(out_decompressed_buf.size()));

  if (decompressed_bytes < 0 ||
      static_cast<size_t>(decompressed_bytes) != out_decompressed_buf.size()) {
    ALOGE(
        "EarlyBootAnimation: LZ4 decompression failed (result=%d, "
        "expected=%zu)",
        decompressed_bytes, out_decompressed_buf.size());
    return false;
  }

  if (!WriteFrameToDmaBuf(out_decompressed_buf, back_idx, bytes_per_pixel)) {
    ALOGE(
        "EarlyBootAnimation: Failed to write decompressed frame to DMA buffer");
    return false;
  }

  return true;
}
#else
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool EarlyBootAnimation::DecompressAndWriteFrame(
    const std::vector<uint8_t>& /*compressed_buf*/,
    uint32_t /*compressed_size*/, int /*back_idx*/,
    uint32_t /*bytes_per_pixel*/,
    std::vector<uint8_t>& /*out_decompressed_buf*/) {
  ALOGE("EarlyBootAnimation: LZ4 support disabled at build time");
  return false;
}
#endif

bool EarlyBootAnimation::WriteFrameToDmaBuf(
    const std::vector<uint8_t>& frame_buf, int back_idx,
    uint32_t bytes_per_pixel) {
  if (back_idx < 0 || static_cast<size_t>(back_idx) >= buffers_.size()) {
    ALOGE("EarlyBootAnimation: Invalid back buffer index %d", back_idx);
    return false;
  }

  auto* dst = static_cast<uint8_t*>(buffers_[back_idx].mmap_addr);
  if (dst == nullptr) {
    ALOGE("EarlyBootAnimation: Null mmap destination pointer for buffer %d",
          back_idx);
    return false;
  }

  const size_t row_bytes = static_cast<size_t>(header_.width) * bytes_per_pixel;
  const size_t required_src_bytes = row_bytes * header_.height;
  if (frame_buf.size() < required_src_bytes) {
    ALOGE("EarlyBootAnimation: Source frame buffer too small (%zu < %zu)",
          frame_buf.size(), required_src_bytes);
    return false;
  }

  const size_t required_dst_size = static_cast<size_t>(offset_y_ +
                                                       header_.height) *
                                   buffers_[back_idx].pitch;
  if (buffers_[back_idx].mmap_size < required_dst_size) {
    ALOGE("EarlyBootAnimation: Destination buffer size too small (%zu < %zu)",
          buffers_[back_idx].mmap_size, required_dst_size);
    return false;
  }

  if ((static_cast<size_t>(offset_x_) * bytes_per_pixel) + row_bytes >
      buffers_[back_idx].pitch) {
    ALOGE("EarlyBootAnimation: Horizontal frame bounds exceed pitch (%zu > %u)",
          (static_cast<size_t>(offset_x_) * bytes_per_pixel) + row_bytes,
          buffers_[back_idx].pitch);
    return false;
  }

  int prime_fd = buffers_[back_idx].prime_fd;
  if (prime_fd < 0) {
    ALOGE("EarlyBootAnimation: Invalid prime_fd");
    return false;
  }

  struct dma_buf_sync sync_args{};

  // DMA_BUF_IOCTL_SYNC ensures CPU cache lines are flushed to device-visible
  // memory for cached DMA buffers.
  sync_args.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
  int ret = 0;
  // DMA_BUF_IOCTL_SYNC can be interrupted by signals (EINTR) or fail with
  // EAGAIN if fences are temporarily busy, so retry until completion or a hard
  // error.
  do {
    // NOLINTNEXTLINE(misc-include-cleaner)
    ret = ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync_args);
  } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
  if (ret < 0) {
    if (errno == ENOTTY) {
      ALOGW("EarlyBootAnimation: DMA-BUF sync ioctl not supported by driver");
    } else {
      ALOGE("EarlyBootAnimation: DMA_BUF_SYNC_START failed (errno=%d)", errno);
      return false;
    }
  }

  // Apply offset to center the animation in the full screen buffer
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  dst += offset_y_ * buffers_[back_idx].pitch + offset_x_ * bytes_per_pixel;

  const uint8_t* src = frame_buf.data();
  const uint32_t pitch = buffers_[back_idx].pitch;

  for (uint32_t y = 0; y < header_.height; ++y) {
    std::memcpy(dst, src, row_bytes);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dst += pitch;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    src += row_bytes;
  }

  sync_args.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
  // Retry on EINTR or EAGAIN until the sync completes or encounters a hard
  // error.
  do {
    // NOLINTNEXTLINE(misc-include-cleaner)
    ret = ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &sync_args);
  } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
  if (ret < 0) {
    if (errno == ENOTTY) {
      ALOGW("EarlyBootAnimation: DMA-BUF sync ioctl not supported by driver");
    } else {
      ALOGE("EarlyBootAnimation: DMA_BUF_SYNC_END failed (errno=%d)", errno);
      return false;
    }
  }

  return true;
}

bool EarlyBootAnimation::PresentFrame(int back_idx,
                                      uint32_t frame_count) const {
  if (state_ != AnimationState::kRunning || display_->IsInHeadlessMode()) {
    ALOGE(
        "EarlyBootAnimation: Attempting to present frames for a stopped "
        "animation or headless display");
    return false;
  }

  const HwcDisplayConfig* current_config = display_->GetCurrentConfig();
  if (current_config == nullptr) {
    ALOGE("EarlyBootAnimation: No active display config for frame %u",
          frame_count);
    return false;
  }

  AtomicCommitArgs
      commit_args = display_
                        ->CreateModesetCommit(current_config,
                                              buffers_[back_idx]
                                                  .hwc_layer->GetLayerData());
  commit_args.display_mode.reset();
  commit_args.seamless = true;
  commit_args.blocking = true;

  if (!commit_args.composition) {
    ALOGE("EarlyBootAnimation: Failed to create composition plan for frame %u",
          frame_count);
    return false;
  }

  auto result = display_->ExecuteAtomicCommit(commit_args);
  if (!result.IsSuccess()) {
    ALOGE("EarlyBootAnimation: Failed to commit frame %u, error_code=%d",
          frame_count, result.GetStatus().error_code);
    return false;
  }
  return true;
}

bool EarlyBootAnimation::AllocateBuffers(
    uint32_t format, const HwcDisplayConfig* current_config) {
  uint32_t drm_format = format;
  if (drm_format == 0) {
    ALOGE("EarlyBootAnimation: Unsupported format %u", format);
    return false;
  }

  uint32_t display_width = current_config->mode.GetRawMode().hdisplay;
  uint32_t display_height = current_config->mode.GetRawMode().vdisplay;

  if (header_.width > display_width || header_.height > display_height) {
    ALOGE("EarlyBootAnimation: Animation size %ux%u exceeds display size %ux%u",
          header_.width, header_.height, display_width, display_height);
    return false;
  }

  offset_x_ = (display_width - header_.width) / 2;
  offset_y_ = (display_height - header_.height) / 2;

  ALOGD(
      "EarlyBootAnimation: Centering %ux%u animation on %ux%u display at "
      "(%u,%u)",
      header_.width, header_.height, display_width, display_height, offset_x_,
      offset_y_);

  DstRectInfo display_frame{
      .i_rect = IRect{.left = 0,
                      .top = 0,
                      .right = static_cast<int32_t>(display_width),
                      .bottom = static_cast<int32_t>(display_height)}};

  SrcRectInfo source_crop{
      .f_rect = FRect{.left = 0.0F,
                      .top = 0.0F,
                      .right = static_cast<float>(display_width),
                      .bottom = static_cast<float>(display_height)}};

  for (size_t i = 0; i < buffers_.size(); ++i) {
    auto bi_opt = display_->GetPipe()
                      .device
                      ->CreateDumbBuffer(display_width, display_height,
                                         drm_format,
                                         // NOLINTNEXTLINE(misc-include-cleaner)
                                         DRM_CLOEXEC | DRM_RDWR);
    if (!bi_opt) {
      ALOGE("EarlyBootAnimation: Failed to allocate dumb buffer %zu", i);
      FreeBuffers();
      return false;
    }
    buffers_[i].bi = bi_opt.value();
    ALOGD(
        "HWC_DUMB_BUFFER: Allocated dumb buffer %zu (width=%u, height=%u, "
        "format=%u, pitch=%u, size=%u)",
        i, buffers_[i].bi.width, buffers_[i].bi.height, buffers_[i].bi.format,
        buffers_[i].bi.pitches[0], buffers_[i].bi.sizes[0]);

    if (buffers_[i].bi.prime_fds[0] >= 0 && buffers_[i].bi.sizes[0] > 0) {
      void* ptr = mmap(nullptr, buffers_[i].bi.sizes[0],
                       // NOLINTNEXTLINE(misc-include-cleaner)
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       buffers_[i].bi.prime_fds[0], 0);
      if (ptr != MAP_FAILED) {
        buffers_[i].mmap_addr = ptr;
        buffers_[i].mmap_size = buffers_[i].bi.sizes[0];
        buffers_[i].pitch = buffers_[i].bi.pitches[0];
        buffers_[i].prime_fd = buffers_[i].bi.prime_fds[0];
        memset(ptr, 0, buffers_[i].bi.sizes[0]);
      } else {
        ALOGE("EarlyBootAnimation: Failed to mmap dumb buffer %zu (errno=%d)",
              i, errno);
        FreeBuffers();
        return false;
      }
    } else {
      ALOGE("EarlyBootAnimation: Invalid fd or size for dumb buffer %zu", i);
      FreeBuffers();
      return false;
    }

    auto fb = display_->GetPipe().importer->GetOrCreateFbId(&buffers_[i].bi);
    if (!fb) {
      ALOGE("EarlyBootAnimation: Failed to get FB for dumb buffer %zu", i);
      FreeBuffers();
      return false;
    }
    buffers_[i].fb_id_handle = fb;

    bool is_hdr_format = IsHdrFormat(header_.format);
    HwcColorspace layer_colorspace = is_hdr_format ? HwcColorspace::kBt2020
                                                   : HwcColorspace::kBt709;
    TransferFunction layer_tf = is_hdr_format ? TransferFunction::kPq
                                              : TransferFunction::kSrgb;
    buffers_[i].hwc_layer = std::make_unique<HwcLayer>(display_);
    buffers_[i].hwc_layer->SetLayerProperties({
        .buffer = std::optional<HwcLayer::Buffer>({
            .bi = buffers_[i].bi,
            .fb = fb,
            .fence = {},
        }),
        .blend_mode = BufferBlendMode::kNone,
        .colorspace = layer_colorspace,
        .transfer_func = layer_tf,
        .display_frame = display_frame,
        .source_crop = source_crop,
    });
  }
  return true;
}

ConfigId EarlyBootAnimation::SelectBestConfig(
    const HwcDisplayConfig* current,
    const std::vector<HwcDisplayConfig>& configs, float anim_fps) {
  if (current == nullptr) {
    return 0;
  }

  std::optional<ConfigId> best_config_id;
  float current_refresh = current->mode.GetVRefresh();
  float max_refresh = 0.F;

  ALOGD(
      "EarlyBootAnimation: SelectBestConfig for anim_fps=%.2f. Current "
      "config=%u, group_id=%u, refresh=%.2f Hz, output_type=%d",
      anim_fps, current->id, current->group_id, current_refresh,
      static_cast<int>(current->output_type));

  // Pass 1: Search for highest refresh rate config strictly within the same
  // group.
  if (current->group_id != 0) {
    for (const auto& config : configs) {
      if (config.group_id == current->group_id &&
          config.output_type == current->output_type) {
        float refresh = config.mode.GetVRefresh();
        if (refresh > max_refresh) {
          max_refresh = refresh;
          best_config_id = config.id;
        }
      }
    }
  }

  // Pass 2: Fallback to same size and output_type across all configs.
  if (!best_config_id) {
    for (const auto& config : configs) {
      if (config.mode.SameSize(current->mode) &&
          config.output_type == current->output_type) {
        float refresh = config.mode.GetVRefresh();
        if (refresh > max_refresh) {
          max_refresh = refresh;
          best_config_id = config.id;
        }
      }
    }
  }

  if (best_config_id) {
    ALOGD(
        "EarlyBootAnimation: SelectBestConfig selected highest refresh rate "
        "config %u "
        "(%.2f Hz)",
        *best_config_id, max_refresh);
    return *best_config_id;
  }

  // Fallback to current if no matching config found
  ALOGW(
      "EarlyBootAnimation: SelectBestConfig could not find a suitable "
      "config. Falling back to current %u",
      current->id);
  return current->id;
}

void EarlyBootAnimation::FreeBuffers() {
  for (auto& buffer : buffers_) {
    if (buffer.hwc_layer) {
      buffer.hwc_layer.reset();
    }
    if (buffer.mmap_addr != nullptr && buffer.mmap_addr != MAP_FAILED) {
      munmap(buffer.mmap_addr, buffer.mmap_size);
      buffer.mmap_addr = nullptr;
    }
    buffer.fb_id_handle.reset();
    buffer.bi = {};
    buffer.prime_fd = -1;
  }
}

}  // namespace android::drm_hwcomposer
