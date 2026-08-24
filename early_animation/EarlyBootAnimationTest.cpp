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

#include "early_animation/EarlyBootAnimation.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <drm_fourcc.h>
#include <fcntl.h>  // IWYU pragma: keep
#if HAS_LZ4
#include <lz4.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#include <xf86drmMode.h>

#include "drm/DrmMode.h"
#include "hwc/HwcDisplayConfigs.h"

namespace android::drm_hwcomposer {

class EarlyBootAnimationTest : public ::testing::Test {
 protected:
  using AnimationHeader = EarlyBootAnimation::AnimationHeader;
  using AnimationState = EarlyBootAnimation::AnimationState;

  void SetUp() override {
    anim_ = std::make_unique<EarlyBootAnimation>(nullptr);
  }

  static android::base::unique_fd CreateSeekableFdWithData(const void* data,
                                                           size_t size) {
    TemporaryFile tf;
    // NOLINTNEXTLINE(misc-include-cleaner)
    int raw_fd = open(tf.path, O_RDWR | O_CLOEXEC);
    if (raw_fd < 0) {
      return {};
    }
    android::base::unique_fd fd(raw_fd);
    if (size > 0 && data != nullptr) {
      if (write(fd.get(), data, size) != static_cast<ssize_t>(size)) {
        return {};
      }
      lseek(fd.get(), 0, SEEK_SET);
    }
    return fd;
  }

  static HwcDisplayConfig MakeConfig(ConfigId id, uint16_t w, uint16_t h,
                                     uint32_t vrefresh, OutputType output_type,
                                     uint32_t group_id = 0) {
    drmModeModeInfo mode_info{};
    mode_info.hdisplay = w;
    mode_info.vdisplay = h;
    mode_info.vrefresh = vrefresh;
    mode_info.clock = 0;
    return HwcDisplayConfig{
        .id = id,
        .group_id = group_id,
        .mode = DrmMode(&mode_info),
        .output_type = output_type,
    };
  }

  bool ReadHeader(int fd) {
    return anim_->ReadHeader(fd);
  }

  bool ReadIndexTable(int fd) {
    return anim_->ReadIndexTable(fd);
  }

  static bool ReadFrame(int fd, size_t frame_size,
                        std::vector<uint8_t>& out_frame_buf) {
    return EarlyBootAnimation::ReadFrame(fd, frame_size, out_frame_buf);
  }

  bool ReadCompressedFrame(int fd, uint32_t frame_idx,
                           std::vector<uint8_t>& out_compressed_buf) {
    return anim_->ReadCompressedFrame(fd, frame_idx, out_compressed_buf);
  }

  bool DecompressAndWriteFrame(const std::vector<uint8_t>& compressed_buf,
                               uint32_t compressed_size, int back_idx,
                               uint32_t bytes_per_pixel,
                               std::vector<uint8_t>& decompressed_buf) {
    return anim_->DecompressAndWriteFrame(compressed_buf, compressed_size,
                                          back_idx, bytes_per_pixel,
                                          decompressed_buf);
  }

  void SetMockBuffer(int idx, void* addr, size_t size, uint32_t pitch,
                     int prime_fd) {
    anim_->buffers_[idx].mmap_addr = addr;
    anim_->buffers_[idx].mmap_size = size;
    anim_->buffers_[idx].pitch = pitch;
    anim_->buffers_[idx].prime_fd = prime_fd;
  }

  void SetStateRunning() {
    anim_->state_ = EarlyBootAnimation::AnimationState::kRunning;
  }

  void HandleFrameHold() {
    anim_->HandleFrameHold();
  }

  void TriggerStop() {
    anim_->Stop();
  }

  void WaitForCompletion() {
    anim_->WaitForCompletion();
  }

  const AnimationHeader& GetHeader() const {
    return anim_->header_;
  }

  void SetHeader(const AnimationHeader& h) {
    anim_->header_ = h;
  }

  bool IsCompressed() const {
    return anim_->compressed_;
  }

  const std::vector<uint32_t>& GetFrameSizes() const {
    return anim_->frame_sizes_;
  }

  const std::vector<uint64_t>& GetFrameOffsets() const {
    return anim_->frame_offsets_;
  }

  static uint32_t CalculateMultiplier(float vrefresh, uint32_t anim_fps) {
    return EarlyBootAnimation::CalculateMultiplier(vrefresh, anim_fps);
  }

  static std::chrono::nanoseconds CalculateFrameDuration(float vrefresh,
                                                         uint32_t anim_fps) {
    return EarlyBootAnimation::CalculateFrameDuration(vrefresh, anim_fps);
  }

  static ConfigId SelectBestConfig(const HwcDisplayConfig* current,
                                   const std::vector<HwcDisplayConfig>& configs,
                                   float anim_fps) {
    return EarlyBootAnimation::SelectBestConfig(current, configs, anim_fps);
  }

 private:
  std::unique_ptr<EarlyBootAnimation> anim_;
};

// NOLINTBEGIN(readability-magic-numbers)
TEST_F(EarlyBootAnimationTest, HeaderStructSizeAndMagic) {
  EXPECT_EQ(sizeof(AnimationHeader), 32U);

  AnimationHeader raw_header{};
  std::memcpy(raw_header.magic.data(), "RAWF", 4);
  EXPECT_EQ(std::memcmp(raw_header.magic.data(), "RAWF", 4), 0);

  AnimationHeader lz4_header{};
  std::memcpy(lz4_header.magic.data(), "LZ4F", 4);
  EXPECT_EQ(std::memcmp(lz4_header.magic.data(), "LZ4F", 4), 0);
}

TEST_F(EarlyBootAnimationTest, ReadHeaderValidRawf) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "RAWF", 4);
  h.width = 1920;
  h.height = 1080;
  h.fps = 60;
  h.num_frames = 120;
  h.format = DRM_FORMAT_ARGB8888;
  h.hold_frame = 74;
  h.hold_duration_ms = 1000;

  android::base::unique_fd fd = CreateSeekableFdWithData(&h, sizeof(h));
  ASSERT_TRUE(fd.ok());

  EXPECT_TRUE(ReadHeader(fd.get()));
  EXPECT_FALSE(IsCompressed());
  EXPECT_EQ(GetHeader().width, 1920U);
  EXPECT_EQ(GetHeader().height, 1080U);
  EXPECT_EQ(GetHeader().fps, 60U);
  EXPECT_EQ(GetHeader().num_frames, 120U);
  EXPECT_EQ(GetHeader().hold_frame, 74U);
  EXPECT_EQ(GetHeader().hold_duration_ms, 1000U);
}

#if HAS_LZ4
TEST_F(EarlyBootAnimationTest, ReadHeaderValidLz4f) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "LZ4F", 4);
  h.width = 2880;
  h.height = 1800;
  h.fps = 60;
  h.num_frames = 121;
  h.format = DRM_FORMAT_ARGB8888;

  android::base::unique_fd fd = CreateSeekableFdWithData(&h, sizeof(h));
  ASSERT_TRUE(fd.ok());

  EXPECT_TRUE(ReadHeader(fd.get()));
  EXPECT_TRUE(IsCompressed());
  EXPECT_EQ(GetHeader().width, 2880U);
}
#else
TEST_F(EarlyBootAnimationTest, ReadHeaderLz4fWithoutLz4SupportFails) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "LZ4F", 4);
  h.width = 2880;
  h.height = 1800;
  h.fps = 60;
  h.num_frames = 121;
  h.format = DRM_FORMAT_ARGB8888;

  android::base::unique_fd fd = CreateSeekableFdWithData(&h, sizeof(h));
  ASSERT_TRUE(fd.ok());

  EXPECT_FALSE(ReadHeader(fd.get()));
}
#endif

TEST_F(EarlyBootAnimationTest, ReadHeaderCorruptMagic) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "BADM", 4);
  h.width = 1920;
  h.height = 1080;
  h.fps = 60;
  h.num_frames = 120;
  h.format = DRM_FORMAT_ARGB8888;

  android::base::unique_fd fd = CreateSeekableFdWithData(&h, sizeof(h));
  ASSERT_TRUE(fd.ok());

  EXPECT_FALSE(ReadHeader(fd.get()));
}

TEST_F(EarlyBootAnimationTest, ReadHeaderInvalidDimensions) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "RAWF", 4);
  h.width = 0;
  h.height = 1080;
  h.fps = 60;
  h.num_frames = 120;
  h.format = DRM_FORMAT_ARGB8888;

  android::base::unique_fd fd = CreateSeekableFdWithData(&h, sizeof(h));
  ASSERT_TRUE(fd.ok());

  EXPECT_FALSE(ReadHeader(fd.get()));
}

TEST_F(EarlyBootAnimationTest, ReadHeaderInvalidFps) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "RAWF", 4);
  h.width = 1920;
  h.height = 1080;
  h.fps = 0;
  h.num_frames = 120;
  h.format = DRM_FORMAT_ARGB8888;

  android::base::unique_fd fd = CreateSeekableFdWithData(&h, sizeof(h));
  ASSERT_TRUE(fd.ok());

  EXPECT_FALSE(ReadHeader(fd.get()));
}

TEST_F(EarlyBootAnimationTest, ReadIndexTableValid) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "LZ4F", 4);
  h.width = 1920;
  h.height = 1080;
  h.fps = 60;
  h.num_frames = 3;
  h.format = DRM_FORMAT_ARGB8888;

  std::vector<uint32_t> frame_sizes = {1000, 2000, 1500};
  std::vector<uint8_t> payload(4500, 0xAB);

  std::vector<uint8_t> full_file;
  full_file.resize(sizeof(h) + (frame_sizes.size() * sizeof(uint32_t)) +
                   payload.size());
  std::memcpy(full_file.data(), &h, sizeof(h));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::memcpy(full_file.data() + sizeof(h), frame_sizes.data(),
              frame_sizes.size() * sizeof(uint32_t));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::memcpy(full_file.data() + sizeof(h) +
                  (frame_sizes.size() * sizeof(uint32_t)),
              payload.data(), payload.size());

  android::base::unique_fd fd = CreateSeekableFdWithData(full_file.data(),
                                                         full_file.size());
  ASSERT_TRUE(fd.ok());

  EXPECT_TRUE(ReadHeader(fd.get()));
  EXPECT_TRUE(ReadIndexTable(fd.get()));

  ASSERT_EQ(GetFrameSizes().size(), 3U);
  EXPECT_EQ(GetFrameSizes()[0], 1000U);
  EXPECT_EQ(GetFrameSizes()[1], 2000U);
  EXPECT_EQ(GetFrameSizes()[2], 1500U);

  ASSERT_EQ(GetFrameOffsets().size(), 3U);
  EXPECT_EQ(GetFrameOffsets()[0], 44U);
  EXPECT_EQ(GetFrameOffsets()[1], 1044U);
  EXPECT_EQ(GetFrameOffsets()[2], 3044U);
}

TEST_F(EarlyBootAnimationTest, ReadIndexTableCorruptFrameSize) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "LZ4F", 4);
  h.width = 100;
  h.height = 100;
  h.fps = 60;
  h.num_frames = 1;
  h.format = DRM_FORMAT_ARGB8888;

  uint32_t huge_size = 10 * 1024 * 1024;
  std::vector<uint8_t> file_buf;
  file_buf.resize(sizeof(h) + sizeof(huge_size));
  std::memcpy(file_buf.data(), &h, sizeof(h));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::memcpy(file_buf.data() + sizeof(h), &huge_size, sizeof(huge_size));

  android::base::unique_fd fd = CreateSeekableFdWithData(file_buf.data(),
                                                         file_buf.size());
  ASSERT_TRUE(fd.ok());

  EXPECT_TRUE(ReadHeader(fd.get()));
  EXPECT_FALSE(ReadIndexTable(fd.get()));
}

TEST_F(EarlyBootAnimationTest, FrameDurationAndMultiplierCalculation) {
  // 60 FPS animation on 60 Hz display -> multiplier 1, ~16.666 ms
  EXPECT_EQ(CalculateMultiplier(60.0F, 60), 1U);
  EXPECT_NEAR(CalculateFrameDuration(60.0F, 60).count(), 16666667, 100);

  // 60 FPS animation on 120 Hz display -> multiplier 2, ~16.666 ms
  EXPECT_EQ(CalculateMultiplier(120.0F, 60), 2U);
  EXPECT_NEAR(CalculateFrameDuration(120.0F, 60).count(), 16666667, 100);

  // 60 FPS animation on 240 Hz display -> multiplier 4, ~16.666 ms
  EXPECT_EQ(CalculateMultiplier(240.0F, 60), 4U);
  EXPECT_NEAR(CalculateFrameDuration(240.0F, 60).count(), 16666667, 100);

  // 60 FPS animation on 59.94 Hz display (NTSC drift prevention)
  EXPECT_EQ(CalculateMultiplier(59.94F, 60), 1U);
  EXPECT_NEAR(CalculateFrameDuration(59.94F, 60).count(), 16683350, 100);
}

TEST_F(EarlyBootAnimationTest, FindBestConfigSelectionLogic) {
  auto current_60hz = MakeConfig(1, 1920, 1080, 60, OutputType::kSdr);
  auto config_120hz = MakeConfig(2, 1920, 1080, 120, OutputType::kSdr);
  auto config_240hz = MakeConfig(3, 1920, 1080, 240, OutputType::kSdr);
  auto config_diff_res = MakeConfig(4, 3840, 2160, 120, OutputType::kSdr);
  auto config_hdr = MakeConfig(5, 1920, 1080, 120, OutputType::kHdr10);

  std::vector<HwcDisplayConfig> all_configs = {current_60hz, config_120hz,
                                               config_240hz, config_diff_res,
                                               config_hdr};

  // 1. Current config is 60 Hz -> selects highest refresh rate in group (240
  // Hz, id = 3)
  EXPECT_EQ(SelectBestConfig(&current_60hz, all_configs, 60.0F), 3);

  // 2. Current config is 75 Hz -> selects highest refresh rate in group (240
  // Hz, id = 3)
  auto current_75hz = MakeConfig(99, 1920, 1080, 75, OutputType::kSdr);
  EXPECT_EQ(SelectBestConfig(&current_75hz, all_configs, 60.0F), 3);

  // 3. Different resolution candidate (4K 120Hz) must NOT be selected for 1080p
  // panel
  std::vector<HwcDisplayConfig> diff_res_only = {current_75hz, config_diff_res};
  EXPECT_EQ(SelectBestConfig(&current_75hz, diff_res_only, 60.0F), 99);

  // 4. Different output type (HDR vs SDR) candidate must NOT be selected
  std::vector<HwcDisplayConfig> hdr_only = {current_75hz, config_hdr};
  EXPECT_EQ(SelectBestConfig(&current_75hz, hdr_only, 60.0F), 99);

  // 5. Pass 1: Prefer highest refresh rate within the SAME group (group 1)
  // even if another group (group 2) has a higher refresh rate
  auto cur_grp1_60hz = MakeConfig(10, 1920, 1080, 60, OutputType::kSdr,
                                  /*group_id=*/1);
  auto grp1_120hz = MakeConfig(11, 1920, 1080, 120, OutputType::kSdr,
                               /*group_id=*/1);
  auto grp2_240hz = MakeConfig(12, 1920, 1080, 240, OutputType::kSdr,
                               /*group_id=*/2);
  std::vector<HwcDisplayConfig> multi_group_configs = {cur_grp1_60hz,
                                                       grp1_120hz, grp2_240hz};
  EXPECT_EQ(SelectBestConfig(&cur_grp1_60hz, multi_group_configs, 60.0F), 11);
}

TEST_F(EarlyBootAnimationTest, ReadRawFrameValid) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "RAWF", 4);
  h.width = 4;
  h.height = 4;
  h.fps = 60;
  h.num_frames = 1;
  h.format = DRM_FORMAT_ARGB8888;

  std::vector<uint8_t> frame_pixels(static_cast<size_t>(4) * 4 * 4, 0x5A);
  std::vector<uint8_t> file_buf;
  file_buf.resize(sizeof(h) + frame_pixels.size());
  std::memcpy(file_buf.data(), &h, sizeof(h));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::memcpy(file_buf.data() + sizeof(h), frame_pixels.data(),
              frame_pixels.size());

  android::base::unique_fd fd = CreateSeekableFdWithData(file_buf.data(),
                                                         file_buf.size());
  ASSERT_TRUE(fd.ok());

  EXPECT_TRUE(ReadHeader(fd.get()));
  std::vector<uint8_t> out_buf(frame_pixels.size());
  EXPECT_TRUE(ReadFrame(fd.get(), frame_pixels.size(), out_buf));
  EXPECT_EQ(out_buf, frame_pixels);
}

#if HAS_LZ4
TEST_F(EarlyBootAnimationTest, DecompressAndWriteFrameLz4) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "LZ4F", 4);
  h.width = 8;
  h.height = 8;
  h.fps = 60;
  h.num_frames = 1;
  h.format = DRM_FORMAT_ARGB8888;
  SetHeader(h);

  uint32_t bpp = 4;
  size_t raw_size = static_cast<size_t>(8) * 8 * bpp;
  std::vector<uint8_t> original_pixels(raw_size);
  for (size_t i = 0; i < raw_size; ++i) {
    original_pixels[i] = static_cast<uint8_t>(i & 0xFF);
  }

  std::vector<uint8_t> compressed_buf(
      LZ4_compressBound(static_cast<int>(raw_size)));
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  int comp_size = LZ4_compress_default(reinterpret_cast<const char*>(
                                           original_pixels.data()),
                                       reinterpret_cast<char*>(
                                           compressed_buf.data()),
                                       static_cast<int>(raw_size),
                                       static_cast<int>(compressed_buf.size()));
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  ASSERT_GT(comp_size, 0);
  compressed_buf.resize(comp_size);

  std::vector<uint8_t> mock_framebuffer(static_cast<size_t>(8) * (8 * bpp),
                                        0x00);
  android::base::unique_fd dummy_prime_fd(
      // NOLINTNEXTLINE(misc-include-cleaner)
      open("/dev/null", O_RDWR | O_CLOEXEC));
  ASSERT_TRUE(dummy_prime_fd.ok());
  SetMockBuffer(0, mock_framebuffer.data(), mock_framebuffer.size(), 8 * bpp,
                dummy_prime_fd.get());

  std::vector<uint8_t> decomp_scratch(raw_size);
  EXPECT_TRUE(DecompressAndWriteFrame(compressed_buf, comp_size, 0, bpp,
                                      decomp_scratch));
  EXPECT_EQ(mock_framebuffer, original_pixels);
}
#endif

TEST_F(EarlyBootAnimationTest, HandleFrameHoldImmediateCancellationOnStop) {
  AnimationHeader h{};
  std::memcpy(h.magic.data(), "RAWF", 4);
  h.width = 100;
  h.height = 100;
  h.fps = 60;
  h.num_frames = 10;
  h.format = DRM_FORMAT_ARGB8888;
  h.hold_frame = 0;
  h.hold_duration_ms = 5000;  // 5 seconds hold
  SetHeader(h);

  SetStateRunning();

  auto start_time = std::chrono::steady_clock::now();
  std::thread stopper([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TriggerStop();
  });

  HandleFrameHold();
  stopper.join();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start_time)
                     .count();

  // Must cancel promptly within < 500ms rather than blocking for 5000ms
  EXPECT_LT(elapsed, 500);
}

TEST_F(EarlyBootAnimationTest, WaitForCompletionUnblocksOnStopOrCompleted) {
  SetStateRunning();
  auto start_time = std::chrono::steady_clock::now();
  std::thread stopper([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TriggerStop();
  });

  WaitForCompletion();
  stopper.join();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start_time)
                     .count();
  EXPECT_LT(elapsed, 500);
}
// NOLINTEND(readability-magic-numbers)

}  // namespace android::drm_hwcomposer
