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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cutils/native_handle.h>
#include <hardware/gralloc.h>
#include <sync/sync.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicTypes.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <utils/Errors.h>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/GrallocBufferCache.h"
#include "bufferinfo/GrallocBufferHandle.h"  // IWYU pragma: keep
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmHwc.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "hwc/HwcWriteback.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {
namespace {

constexpr ILayerId kFirstLayerId = 1;
constexpr ILayerId kSecondLayerId = 2;
constexpr uint32_t kBaseZOrder = 0;
constexpr uint32_t kHalfDivisor = 2;
constexpr uint32_t kQuarterDivisor = 4;
constexpr uint32_t kThreeQuartersMultiplier = 3;
constexpr uint8_t kMaxColorChannel = 255;
constexpr uint32_t kRgbaBytesPerPixel = 4;
constexpr size_t kRedChannelOffset = 0;
constexpr size_t kGreenChannelOffset = 1;
constexpr size_t kBlueChannelOffset = 2;

// Test harness implementation of DrmHwc that captures hotplug, vsync, and
// refresh callbacks from ResourceManager and SdmBackend without requiring a
// full HWC3 ComposerClient binder service.
class TestDrmHwc : public DrmHwc {
 public:
  bool HasCallback() const override {
    return true;
  }

  void SendVsyncEventToClient(DisplayHandle /*display_handle*/,
                              int64_t /*timestamp*/,
                              uint32_t /*vsync_period*/) const override {
    ++vsync_count_;
  }

  void SendVsyncPeriodTimingChangedEventToClient(
      DisplayHandle /*display_handle*/, int64_t /*timestamp*/) const override {
  }

  void SendRefreshEventToClient(DisplayHandle /*display_handle*/) override {
    ++refresh_count_;
  }

  // Records hotplug events emitted by the backend during initialization and
  // notifies any thread waiting in WaitForPrimaryConnected().
  void SendHotplugEventToClient(DisplayHandle display_handle,
                                DisplayStatus display_status) override {
    {
      std::scoped_lock lock(hotplug_mutex_);
      hotplug_events_.emplace_back(display_handle, display_status);
      if (display_handle == kPrimaryDisplay &&
          display_status == DisplayStatus::kConnected) {
        primary_connected_ = true;
      }
    }
    hotplug_cv_.notify_all();
  }

  void SendHdcpLevelsChangedEventToClient(
      DisplayHandle /*display_handle*/,
      std::optional<HdcpContentType> /*current_hdcp_level*/) override {
  }

  // Blocks until the primary display reports a connected hotplug status or the
  // specified timeout elapses.
  bool WaitForPrimaryConnected(std::chrono::milliseconds timeout) {
    std::unique_lock lock(hotplug_mutex_);
    return hotplug_cv_.wait_for(lock, timeout,
                                [this] { return primary_connected_; });
  }

  int GetVsyncCount() const {
    return vsync_count_.load();
  }

 private:
  mutable std::atomic<int> vsync_count_{0};
  std::atomic<int> refresh_count_{0};
  std::mutex hotplug_mutex_;
  std::condition_variable hotplug_cv_;
  std::vector<std::pair<DisplayHandle, DisplayStatus>> hotplug_events_;
  bool primary_connected_ = false;
};

struct RgbaColor {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = kMaxColorChannel;
};

constexpr RgbaColor kSolidRed{
    .r = kMaxColorChannel,
    .g = 0,
    .b = 0,
    .a = kMaxColorChannel,
};
constexpr RgbaColor kSolidGreen{
    .r = 0,
    .g = kMaxColorChannel,
    .b = 0,
    .a = kMaxColorChannel,
};
constexpr RgbaColor kSolidBlue{
    .r = 0,
    .g = 0,
    .b = kMaxColorChannel,
    .a = kMaxColorChannel,
};
constexpr RgbaColor kSolidMagenta{
    .r = kMaxColorChannel,
    .g = 0,
    .b = kMaxColorChannel,
    .a = kMaxColorChannel,
};

struct RgbPixel {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

// RAII wrapper for a buffer_handle_t imported via GraphicBufferMapper.
struct ScopedMappedBuffer {
  buffer_handle_t handle = nullptr;

  ScopedMappedBuffer() = default;
  ScopedMappedBuffer(const ScopedMappedBuffer&) = delete;
  ScopedMappedBuffer& operator=(const ScopedMappedBuffer&) = delete;

  ~ScopedMappedBuffer() {
    if (handle != nullptr) {
      android::GraphicBufferMapper::get().freeBuffer(handle);
    }
  }
};

std::optional<android::PixelFormat> BufferFormatToHalPixelFormat(
    BufferFormat format) {
  switch (format) {
    case BufferFormat::kRgba8888:
      return android::PIXEL_FORMAT_RGBA_8888;
    case BufferFormat::kXrgb8888:
      return android::PIXEL_FORMAT_RGBX_8888;
    case BufferFormat::kRgbaFp16:
      return android::PIXEL_FORMAT_RGBA_FP16;
    case BufferFormat::kRgba1010102:
      return android::PIXEL_FORMAT_RGBA_1010102;
    case BufferFormat::kUndefined:
      return std::nullopt;
  }
  return std::nullopt;
}

uint32_t BytesPerPixelForFormat(BufferFormat format) {
  switch (format) {
    case BufferFormat::kRgba8888:
    case BufferFormat::kXrgb8888:
    case BufferFormat::kRgba1010102:
      return kRgbaBytesPerPixel;
    case BufferFormat::kRgbaFp16:
    case BufferFormat::kUndefined:
      return 0;
  }
  return 0;
}

// Move-only RAII wrapper around a raw gralloc buffer_handle_t allocated via
// GraphicBufferAllocator::allocateRawHandle(). Closes file descriptors and
// deletes the native_handle_t on destruction so test buffers are not leaked.
struct AllocatedBuffer {
  buffer_handle_t raw_handle = nullptr;
  uint32_t stride = 0;

  AllocatedBuffer() = default;
  AllocatedBuffer(const AllocatedBuffer&) = delete;
  AllocatedBuffer& operator=(const AllocatedBuffer&) = delete;

  AllocatedBuffer(AllocatedBuffer&& other) noexcept
      : raw_handle(other.raw_handle), stride(other.stride) {
    other.raw_handle = nullptr;
    other.stride = 0;
  }

  AllocatedBuffer& operator=(AllocatedBuffer&& other) noexcept {
    if (this != &other) {
      Reset();
      raw_handle = other.raw_handle;
      stride = other.stride;
      other.raw_handle = nullptr;
      other.stride = 0;
    }
    return *this;
  }

  ~AllocatedBuffer() {
    Reset();
  }

  bool IsValid() const {
    return raw_handle != nullptr;
  }

  void Reset() {
    if (raw_handle != nullptr) {
      native_handle_close(raw_handle);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      native_handle_delete(const_cast<native_handle_t*>(raw_handle));
      raw_handle = nullptr;
    }
  }
};

struct SdmBackendTest : public ::testing::Test {
  static void SetUpTestSuite() {
    constexpr int kHotplugTimeoutMs = 5000;
    hwc = std::make_unique<TestDrmHwc>();
    hwc->GetResMan().Init();

    // SdmBackend and libsdmclient hold process-global singletons whose C++
    // static destructors run out of order during __cxa_finalize (destroying
    // SDMLayerBuilder::locker_ before SDMInterfaceFactoryImpl::factory_ and
    // SdmBackend::sideband_callbacks before SdmBackend::life_cycle_intf).
    // Register an atexit handler after SDM initialization so that once
    // RUN_ALL_TESTS() and TearDownTestSuite() have completed, the process
    // flushes output and exits cleanly before static destruction.
    std::atexit([]() {
      // NOLINTNEXTLINE(misc-include-cleaner)
      std::fflush(nullptr);
      const auto* unit_test = ::testing::UnitTest::GetInstance();
      const bool passed = (unit_test != nullptr) && unit_test->Passed();
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      std::_Exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
    });

    ASSERT_TRUE(hwc->WaitForPrimaryConnected(
        std::chrono::milliseconds(kHotplugTimeoutMs)))
        << "Timed out waiting for primary display hotplug from SdmBackend";
  }

  static void TearDownTestSuite() {
    if (hwc != nullptr) {
      hwc->GetResMan().DeInit();
      hwc.reset();
    }
  }

  void SetUp() override {
    ASSERT_NE(hwc, nullptr);
    std::scoped_lock lock(hwc->GetResMan().GetMainLock());
    primary_display = hwc->GetDisplay(kPrimaryDisplay);
    ASSERT_NE(primary_display, nullptr);
    ASSERT_FALSE(primary_display->IsInHeadlessMode());

    primary_display->SetColorMode(ColorMode::kNative,
                                  ui::RenderIntent::COLORIMETRIC);
    ASSERT_EQ(primary_display->SetPowerMode(PowerMode::kOn),
              HwcDisplay::Error::kNone);

    const HwcDisplayConfig* active_config = primary_display->GetCurrentConfig();
    ASSERT_NE(active_config, nullptr);
    display_width = active_config->mode.GetRawMode().hdisplay;
    display_height = active_config->mode.GetRawMode().vdisplay;
    ASSERT_GT(display_width, 0U);
    ASSERT_GT(display_height, 0U);

    buffer_cache = std::make_unique<GrallocBufferCache>(
        [this](BufferInfo& bi) -> std::shared_ptr<DrmFbIdHandle> {
          if (primary_display->IsInHeadlessMode()) {
            return nullptr;
          }
          return primary_display->GetPipe().importer->GetOrCreateFbId(&bi);
        });
  }

  void TearDown() override {
    if (primary_display != nullptr && hwc != nullptr) {
      std::scoped_lock lock(hwc->GetResMan().GetMainLock());
      std::vector<ILayerId> active_layers;
      active_layers.reserve(primary_display->layers().size());
      for (const auto& [id, layer] : primary_display->layers()) {
        active_layers.push_back(id);
      }
      for (const ILayerId id : active_layers) {
        primary_display->DestroyLayer(id);
      }
      // Clear any stale writeback fence and reset the client layer buffer.
      primary_display->TakeWritebackBufferFence();
      primary_display->SetColorMode(ColorMode::kNative,
                                    ui::RenderIntent::COLORIMETRIC);
      primary_display->SetPowerMode(PowerMode::kOn);
    }
    buffer_cache.reset();
    primary_display = nullptr;
  }

  // Allocates a raw gralloc buffer handle via GraphicBufferAllocator without
  // GraphicBuffer / Mapper v5 dependencies so vendor test binaries can allocate
  // hardware composer buffers directly.
  static AllocatedBuffer AllocateBuffer(
      uint32_t width, uint32_t height,
      android::PixelFormat format = android::PIXEL_FORMAT_RGBA_8888,
      uint64_t usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER |
                       GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_SW_WRITE_OFTEN |
                       GRALLOC_USAGE_SW_READ_OFTEN) {
    constexpr uint32_t kSingleLayerCount = 1;
    AllocatedBuffer buf;
    android::status_t status = android::GraphicBufferAllocator::get()
                                   .allocateRawHandle(width, height, format,
                                                      kSingleLayerCount, usage,
                                                      &buf.raw_handle,
                                                      &buf.stride,
                                                      "SdmBackendTest");
    if (status != android::NO_ERROR || buf.raw_handle == nullptr) {
      return {};
    }
    return buf;
  }

  // Allocates a display-sized gralloc buffer suitable for concurrent writeback
  // capture.
  AllocatedBuffer AllocateWritebackBuffer(BufferFormat format) const {
    constexpr uint64_t kWritebackBufferUsage = GRALLOC_USAGE_HW_COMPOSER |
                                               GRALLOC_USAGE_HW_TEXTURE |
                                               GRALLOC_USAGE_SW_READ_OFTEN;
    const auto hal_format = BufferFormatToHalPixelFormat(format);
    if (!hal_format.has_value()) {
      return {};
    }
    return AllocateBuffer(display_width, display_height, *hal_format,
                          kWritebackBufferUsage);
  }

  BufferFormat GetPrimaryWritebackFormat() const {
    const auto attrs = primary_display->GetWritebackBufferAttributes();
    if (!attrs.has_value()) {
      return BufferFormat::kUndefined;
    }
    return attrs->format;
  }

  // Locks an RGBA_8888 gralloc buffer via GraphicBufferMapper and fills every
  // pixel with the specified color.
  static bool FillRgbaBuffer(const AllocatedBuffer& buf, uint32_t width,
                             uint32_t height, RgbaColor color) {
    constexpr size_t kAlphaChannelOffset = 3;
    auto& mapper = android::GraphicBufferMapper::get();
    ScopedMappedBuffer mapped;
    if (mapper.importBufferNoValidate(buf.raw_handle, &mapped.handle) !=
        android::NO_ERROR) {
      return false;
    }

    uint32_t stride_bytes = buf.stride * kRgbaBytesPerPixel;
    std::vector<android::ui::PlaneLayout> layouts;
    if (mapper.getPlaneLayouts(mapped.handle, &layouts) == android::NO_ERROR &&
        !layouts.empty() && layouts[0].strideInBytes > 0) {
      stride_bytes = static_cast<uint32_t>(layouts[0].strideInBytes);
    }

    void* vaddr = nullptr;
    const android::Rect bounds(static_cast<int32_t>(width),
                               static_cast<int32_t>(height));
    if (mapper.lock(mapped.handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds,
                    &vaddr) != android::NO_ERROR ||
        vaddr == nullptr) {
      return false;
    }

    std::vector<uint8_t> staging(static_cast<size_t>(height) * stride_bytes, 0);
    for (uint32_t y = 0; y < height; ++y) {
      const size_t row_offset = static_cast<size_t>(y) * stride_bytes;
      for (uint32_t x = 0; x < width; ++x) {
        const size_t px_offset = row_offset +
                                 (static_cast<size_t>(x) * kRgbaBytesPerPixel);
        staging[px_offset + kRedChannelOffset] = color.r;
        staging[px_offset + kGreenChannelOffset] = color.g;
        staging[px_offset + kBlueChannelOffset] = color.b;
        staging[px_offset + kAlphaChannelOffset] = color.a;
      }
    }
    std::memcpy(vaddr, staging.data(), staging.size());

    mapper.unlock(mapped.handle);
    return true;
  }

  // Imports a writeback buffer and stages it on the primary display for
  // one-shot writeback.
  bool StageWritebackBuffer(const AllocatedBuffer& writeback_buf) const {
    if (!primary_display->GetWritebackBufferAttributes().has_value()) {
      return false;
    }
    auto imported_handle = BufferInfoGetter::GetInstance()->Import(
        writeback_buf.raw_handle);
    if (!imported_handle) {
      return false;
    }
    auto bi = BufferInfoGetter::GetInstance()->GetBoInfo(
        imported_handle->GetHandle());
    if (!bi.has_value()) {
      return false;
    }
    bi->fds_shared = imported_handle;

    WritebackBuffer buffer{
        .bi = *bi,
        .fb = primary_display->GetPipe().importer->GetOrCreateFbId(&*bi),
        .fence = nullptr,
    };
    return primary_display->SetWritebackBuffer(buffer);
  }

  static bool WaitForFence(const SharedFd& fence) {
    constexpr int kFenceWaitTimeoutMs = 3000;
    if (!fence || *fence < 0) {
      return false;
    }
    return sync_wait(*fence, kFenceWaitTimeoutMs) == 0;
  }

  // Retrieves the writeback completion fence from the last presented frame and
  // blocks until the fence signals.
  bool WaitForWritebackFromLastPresent() const {
    return WaitForFence(primary_display->TakeWritebackBufferFence());
  }

  // Maps the writeback buffer for CPU read access and returns the RGB pixel at
  // (x, y) after accounting for plane layout stride and sample increment.
  std::optional<RgbPixel> ReadPixelFromWritebackBuffer(
      const AllocatedBuffer& writeback_buf, BufferFormat format, uint32_t x,
      uint32_t y) const {
    constexpr uint32_t kBitsPerByte = 8;
    const uint32_t bpp = BytesPerPixelForFormat(format);
    if (bpp == 0 || x >= display_width || y >= display_height) {
      return std::nullopt;
    }

    auto& mapper = android::GraphicBufferMapper::get();
    ScopedMappedBuffer mapped;
    if (mapper.importBufferNoValidate(writeback_buf.raw_handle,
                                      &mapped.handle) != android::NO_ERROR) {
      return std::nullopt;
    }

    uint32_t stride_bytes = writeback_buf.stride * bpp;
    uint32_t sample_bytes = bpp;
    std::vector<android::ui::PlaneLayout> layouts;
    if (mapper.getPlaneLayouts(mapped.handle, &layouts) == android::NO_ERROR &&
        !layouts.empty()) {
      if (layouts[0].strideInBytes > 0) {
        stride_bytes = static_cast<uint32_t>(layouts[0].strideInBytes);
      }
      if (layouts[0].sampleIncrementInBits >=
          static_cast<int64_t>(kBitsPerByte)) {
        sample_bytes = static_cast<uint32_t>(layouts[0].sampleIncrementInBits /
                                             kBitsPerByte);
      }
    }

    void* vaddr = nullptr;
    const android::Rect bounds(static_cast<int32_t>(display_width),
                               static_cast<int32_t>(display_height));
    if (mapper.lock(mapped.handle, GRALLOC_USAGE_SW_READ_OFTEN, bounds,
                    &vaddr) != android::NO_ERROR ||
        vaddr == nullptr) {
      return std::nullopt;
    }

    const size_t offset = (static_cast<size_t>(y) * stride_bytes) +
                          (static_cast<size_t>(x) * sample_bytes);
    std::vector<uint8_t> staging(offset + bpp, 0);
    std::memcpy(staging.data(), vaddr, staging.size());
    RgbPixel pixel{
        .r = staging[offset + kRedChannelOffset],
        .g = staging[offset + kGreenChannelOffset],
        .b = staging[offset + kBlueChannelOffset],
    };

    mapper.unlock(mapped.handle);
    return pixel;
  }

  // Waits for the writeback completion fence from the last presented frame and
  // reads the RGB pixel value at (x, y) from the writeback buffer.
  std::optional<RgbPixel> ReadWritebackPixelFromLastPresent(
      const AllocatedBuffer& writeback_buf, BufferFormat format, uint32_t x,
      uint32_t y) const {
    if (!WaitForWritebackFromLastPresent()) {
      return std::nullopt;
    }
    return ReadPixelFromWritebackBuffer(writeback_buf, format, x, y);
  }

  static void ExpectPixelEq(std::optional<RgbPixel> actual,
                            RgbaColor expected) {
    ASSERT_TRUE(actual.has_value());
    if (!actual.has_value()) {
      return;
    }
    EXPECT_EQ(actual->r, expected.r);
    EXPECT_EQ(actual->g, expected.g);
    EXPECT_EQ(actual->b, expected.b);
  }

  // Imports an allocated gralloc buffer into the shared GrallocBufferCache
  // using a unique slot and returns the resulting HwcLayer::Buffer.
  std::optional<HwcLayer::Buffer> ImportLayerBuffer(
      const AllocatedBuffer& buf) const {
    return buffer_cache->HandleNextBuffer(buf.raw_handle, /*fence_fd=*/nullptr,
                                          next_buffer_slot++);
  }

  DstRectInfo FullscreenDisplayFrame() const {
    return DstRectInfo{.i_rect = IRect{
                           .left = 0,
                           .top = 0,
                           .right = static_cast<int32_t>(display_width),
                           .bottom = static_cast<int32_t>(display_height),
                       }};
  }

  SrcRectInfo FullscreenSourceCrop() const {
    return SrcRectInfo{.f_rect = FRect{
                           .left = 0.0F,
                           .top = 0.0F,
                           .right = static_cast<float>(display_width),
                           .bottom = static_cast<float>(display_height),
                       }};
  }

  // Configures the display's client target layer with a fullscreen RGBA buffer
  // so client-composited frames can be validated and presented.
  bool SetFullscreenClientTarget(const AllocatedBuffer& client_buf) const {
    HwcLayer& client_layer = primary_display->GetClientLayer();
    auto imported = ImportLayerBuffer(client_buf);
    if (!imported.has_value()) {
      return false;
    }
    HwcLayer::LayerProperties props;
    props.buffer = imported;
    props.blend_mode = BufferBlendMode::kPreMult;
    props.colorspace = HwcColorspace::kBt709;
    props.sample_range = BufferSampleRange::kFullRange;
    props.transfer_func = TransferFunction::kSrgb;
    client_layer.SetLayerProperties(props);
    return true;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  inline static std::unique_ptr<TestDrmHwc> hwc;
  HwcDisplay* primary_display = nullptr;
  uint32_t display_width = 0;
  uint32_t display_height = 0;
  std::unique_ptr<GrallocBufferCache> buffer_cache;
  mutable int32_t next_buffer_slot = 0;
};

// Verifies that SdmBackend initializes the primary internal display, exposes
// display configs and color modes, transitions between power modes, and
// produces non-empty diagnostic state dumps.
TEST_F(SdmBackendTest, PrimaryDisplayInitializationAndPowerModes) {
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  EXPECT_TRUE(primary_display->GetDisplayEnabled());
  EXPECT_EQ(primary_display->GetDisplayType(),
            HwcDisplay::DisplayType::kInternal);

  const auto configs = primary_display->GetDisplayConfigs();
  EXPECT_FALSE(configs.empty());

  const auto color_modes = primary_display->GetColorModes();
  EXPECT_FALSE(color_modes.empty());
  EXPECT_NE(std::find(color_modes.begin(), color_modes.end(),
                      ColorMode::kNative),
            color_modes.end());

  EXPECT_EQ(primary_display->SetPowerMode(PowerMode::kOff),
            HwcDisplay::Error::kNone);
  EXPECT_FALSE(primary_display->GetDisplayEnabled());

  EXPECT_EQ(primary_display->SetPowerMode(PowerMode::kOn),
            HwcDisplay::Error::kNone);
  EXPECT_TRUE(primary_display->GetDisplayEnabled());

  EXPECT_EQ(primary_display->SetPowerMode(PowerMode::kDoze),
            HwcDisplay::Error::kUnsupported);

  EXPECT_FALSE(hwc->DumpState().empty());
  const auto backend_dump = hwc->GetResMan().DumpBackends();
  ASSERT_TRUE(backend_dump.has_value());
  EXPECT_FALSE(backend_dump.value_or(std::string{}).empty());
}

// Verifies that a single fullscreen device-composited layer is validated as
// kDevice, presented by SdmBackend, and captured via writeback with the
// expected pixel color.
TEST_F(SdmBackendTest, SingleDeviceLayerValidateAndPresent) {
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  const BufferFormat writeback_format = GetPrimaryWritebackFormat();
  ASSERT_NE(writeback_format, BufferFormat::kUndefined);

  ASSERT_TRUE(primary_display->CreateLayer(kFirstLayerId));
  HwcLayer* layer = primary_display->get_layer(kFirstLayerId);
  ASSERT_NE(layer, nullptr);

  auto allocated_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(allocated_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(allocated_buf, display_width, display_height, kSolidRed));
  auto client_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(client_buf.IsValid());
  ASSERT_TRUE(SetFullscreenClientTarget(client_buf));
  auto writeback_buf = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf));
  auto imported_buf = ImportLayerBuffer(allocated_buf);
  ASSERT_TRUE(imported_buf.has_value());

  HwcLayer::LayerProperties props;
  props.buffer = imported_buf;
  props.blend_mode = BufferBlendMode::kNone;
  props.colorspace = HwcColorspace::kBt709;
  props.sample_range = BufferSampleRange::kFullRange;
  props.transfer_func = TransferFunction::kSrgb;
  props.composition_type = CompositionType::kDevice;
  props.display_frame = FullscreenDisplayFrame();
  props.source_crop = FullscreenSourceCrop();
  props.z_order = kBaseZOrder;
  props.alpha = kAlphaOpaque;
  layer->SetLayerProperties(props);

  auto validate_result = primary_display->ValidateStagedComposition();
  EXPECT_TRUE(validate_result.changed_layers.empty());
  EXPECT_EQ(layer->GetValidatedType(), CompositionType::kDevice);

  primary_display->AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence,
                                                        release_fences));

  ExpectPixelEq(ReadWritebackPixelFromLastPresent(writeback_buf,
                                                  writeback_format,
                                                  display_width / kHalfDivisor,
                                                  display_height /
                                                      kHalfDivisor),
                kSolidRed);
}

// Verifies that two overlapping layers with different z-orders and
// premultiplied plane alpha blending are composited by the device and produce
// the expected blended and unblended pixel colors on writeback.
TEST_F(SdmBackendTest, MultiLayerDeviceCompositionWithBlendingAndZOrder) {
  constexpr float kTranslucentAlpha = 0.8F;
  constexpr uint8_t kBlendedRedChannel = 204;
  constexpr uint8_t kBlendedBlueChannel = 51;
  constexpr RgbaColor kBlendedRedOverBlue{
      .r = kBlendedRedChannel,
      .g = 0,
      .b = kBlendedBlueChannel,
      .a = kMaxColorChannel,
  };
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  const BufferFormat writeback_format = GetPrimaryWritebackFormat();
  ASSERT_NE(writeback_format, BufferFormat::kUndefined);

  ASSERT_TRUE(primary_display->CreateLayer(kFirstLayerId));
  ASSERT_TRUE(primary_display->CreateLayer(kSecondLayerId));
  HwcLayer* base_layer = primary_display->get_layer(kFirstLayerId);
  HwcLayer* overlay_layer = primary_display->get_layer(kSecondLayerId);
  ASSERT_NE(base_layer, nullptr);
  ASSERT_NE(overlay_layer, nullptr);

  auto base_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(base_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(base_buf, display_width, display_height, kSolidBlue));
  const uint32_t overlay_width = display_width / kHalfDivisor;
  const uint32_t overlay_height = display_height / kHalfDivisor;
  auto overlay_buf = AllocateBuffer(overlay_width, overlay_height);
  ASSERT_TRUE(overlay_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(overlay_buf, overlay_width, overlay_height, kSolidRed));
  auto client_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(client_buf.IsValid());
  ASSERT_TRUE(SetFullscreenClientTarget(client_buf));
  auto writeback_buf = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf));

  auto imported_base = ImportLayerBuffer(base_buf);
  ASSERT_TRUE(imported_base.has_value());
  HwcLayer::LayerProperties base_props;
  base_props.buffer = imported_base;
  base_props.blend_mode = BufferBlendMode::kNone;
  base_props.colorspace = HwcColorspace::kBt709;
  base_props.sample_range = BufferSampleRange::kFullRange;
  base_props.transfer_func = TransferFunction::kSrgb;
  base_props.composition_type = CompositionType::kDevice;
  base_props.display_frame = FullscreenDisplayFrame();
  base_props.source_crop = FullscreenSourceCrop();
  base_props.z_order = kBaseZOrder;
  base_props.alpha = kAlphaOpaque;
  base_layer->SetLayerProperties(base_props);

  auto imported_overlay = ImportLayerBuffer(overlay_buf);
  ASSERT_TRUE(imported_overlay.has_value());
  HwcLayer::LayerProperties overlay_props;
  overlay_props.buffer = imported_overlay;
  overlay_props.blend_mode = BufferBlendMode::kPreMult;
  overlay_props.colorspace = HwcColorspace::kBt709;
  overlay_props.sample_range = BufferSampleRange::kFullRange;
  overlay_props.transfer_func = TransferFunction::kSrgb;
  overlay_props.composition_type = CompositionType::kDevice;
  overlay_props.display_frame = DstRectInfo{
      .i_rect = IRect{
          .left = 0,
          .top = 0,
          .right = static_cast<int32_t>(overlay_width),
          .bottom = static_cast<int32_t>(overlay_height),
      }};
  overlay_props.source_crop = SrcRectInfo{
      .f_rect = FRect{
          .left = 0.0F,
          .top = 0.0F,
          .right = static_cast<float>(overlay_width),
          .bottom = static_cast<float>(overlay_height),
      }};
  overlay_props.z_order = kBaseZOrder + 1;
  overlay_props.alpha = kTranslucentAlpha;
  overlay_layer->SetLayerProperties(overlay_props);

  const auto ordered_layers = primary_display->GetOrderLayersByZPos();
  ASSERT_EQ(ordered_layers.size(), 2U);
  EXPECT_EQ(ordered_layers[0], base_layer);
  EXPECT_EQ(ordered_layers[1], overlay_layer);

  auto validate_result = primary_display->ValidateStagedComposition();
  EXPECT_TRUE(validate_result.changed_layers.empty());
  EXPECT_EQ(base_layer->GetValidatedType(), CompositionType::kDevice);
  EXPECT_EQ(overlay_layer->GetValidatedType(), CompositionType::kDevice);

  primary_display->AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence,
                                                        release_fences));

  ASSERT_TRUE(WaitForWritebackFromLastPresent());
  ExpectPixelEq(ReadPixelFromWritebackBuffer(writeback_buf, writeback_format,
                                             overlay_width / kQuarterDivisor,
                                             overlay_height / kQuarterDivisor),
                kBlendedRedOverBlue);

  ExpectPixelEq(ReadPixelFromWritebackBuffer(writeback_buf, writeback_format,
                                             (display_width *
                                              kThreeQuartersMultiplier) /
                                                 kQuarterDivisor,
                                             (display_height *
                                              kThreeQuartersMultiplier) /
                                                 kQuarterDivisor),
                kSolidBlue);
}

// Verifies that a layer explicitly marked for client composition presents the
// client target buffer rather than the layer's own buffer.
TEST_F(SdmBackendTest, ClientCompositionWithClientTarget) {
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  const BufferFormat writeback_format = GetPrimaryWritebackFormat();
  ASSERT_NE(writeback_format, BufferFormat::kUndefined);

  ASSERT_TRUE(primary_display->CreateLayer(kFirstLayerId));
  HwcLayer* layer = primary_display->get_layer(kFirstLayerId);
  ASSERT_NE(layer, nullptr);

  auto layer_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(layer_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(layer_buf, display_width, display_height, kSolidGreen));
  auto client_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(client_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(client_buf, display_width, display_height, kSolidMagenta));
  ASSERT_TRUE(SetFullscreenClientTarget(client_buf));
  auto writeback_buf = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf));

  auto imported_layer = ImportLayerBuffer(layer_buf);
  ASSERT_TRUE(imported_layer.has_value());

  HwcLayer::LayerProperties props;
  props.buffer = imported_layer;
  props.blend_mode = BufferBlendMode::kPreMult;
  props.colorspace = HwcColorspace::kBt709;
  props.sample_range = BufferSampleRange::kFullRange;
  props.transfer_func = TransferFunction::kSrgb;
  props.composition_type = CompositionType::kClient;
  props.display_frame = FullscreenDisplayFrame();
  props.source_crop = FullscreenSourceCrop();
  props.z_order = kBaseZOrder;
  props.alpha = kAlphaOpaque;
  layer->SetLayerProperties(props);

  auto validate_result = primary_display->ValidateStagedComposition();
  EXPECT_TRUE(validate_result.changed_layers.empty());
  EXPECT_EQ(layer->GetValidatedType(), CompositionType::kClient);
  EXPECT_TRUE(primary_display->NeedsClientLayerUpdate());

  primary_display->AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence,
                                                        release_fences));

  ExpectPixelEq(ReadWritebackPixelFromLastPresent(writeback_buf,
                                                  writeback_format,
                                                  display_width / kHalfDivisor,
                                                  display_height /
                                                      kHalfDivisor),
                kSolidMagenta);
}

// Verifies that a solid-color layer without a buffer falls back to client
// composition and presents the client target buffer.
TEST_F(SdmBackendTest, SolidColorLayerFallsBackToClientComposition) {
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  const BufferFormat writeback_format = GetPrimaryWritebackFormat();
  ASSERT_NE(writeback_format, BufferFormat::kUndefined);

  ASSERT_TRUE(primary_display->CreateLayer(kFirstLayerId));
  HwcLayer* layer = primary_display->get_layer(kFirstLayerId);
  ASSERT_NE(layer, nullptr);

  auto client_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(client_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(client_buf, display_width, display_height, kSolidRed));
  ASSERT_TRUE(SetFullscreenClientTarget(client_buf));
  auto writeback_buf = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf));

  constexpr float kFullChannelIntensity = 1.0F;
  HwcLayer::LayerProperties props;
  props.blend_mode = BufferBlendMode::kPreMult;
  props.composition_type = CompositionType::kSolidColor;
  props.solid_color = SolidColor{
      .r = kFullChannelIntensity,
      .g = 0.0F,
      .b = 0.0F,
      .a = kAlphaOpaque,
  };
  props.display_frame = FullscreenDisplayFrame();
  props.z_order = kBaseZOrder;
  props.alpha = kAlphaOpaque;
  layer->SetLayerProperties(props);

  auto validate_result = primary_display->ValidateStagedComposition();
  ASSERT_EQ(validate_result.changed_layers.size(), 1U);
  EXPECT_EQ(validate_result.changed_layers[0].first, kFirstLayerId);
  EXPECT_EQ(validate_result.changed_layers[0].second, CompositionType::kClient);
  EXPECT_EQ(layer->GetValidatedType(), CompositionType::kClient);

  primary_display->AcceptValidatedComposition();
  EXPECT_EQ(layer->GetSfType(), CompositionType::kClient);

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence,
                                                        release_fences));

  ExpectPixelEq(ReadWritebackPixelFromLastPresent(writeback_buf,
                                                  writeback_format,
                                                  display_width / kHalfDivisor,
                                                  display_height /
                                                      kHalfDivisor),
                kSolidRed);
}

// Verifies multi-frame layer lifecycle management by presenting two layers in
// frame 1, destroying the overlay layer, and presenting an updated buffer on
// the remaining layer in frame 2, checking writeback pixel output on both
// frames.
TEST_F(SdmBackendTest, LayerLifecycleAcrossMultipleFrames) {
  constexpr RgbaColor kSolidYellow{
      .r = kMaxColorChannel,
      .g = kMaxColorChannel,
      .b = 0,
      .a = kMaxColorChannel,
  };
  constexpr RgbaColor kSolidCyan{
      .r = 0,
      .g = kMaxColorChannel,
      .b = kMaxColorChannel,
      .a = kMaxColorChannel,
  };
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  const BufferFormat writeback_format = GetPrimaryWritebackFormat();
  ASSERT_NE(writeback_format, BufferFormat::kUndefined);

  ASSERT_TRUE(primary_display->CreateLayer(kFirstLayerId));
  ASSERT_TRUE(primary_display->CreateLayer(kSecondLayerId));
  HwcLayer* layer_one = primary_display->get_layer(kFirstLayerId);
  HwcLayer* layer_two = primary_display->get_layer(kSecondLayerId);
  ASSERT_NE(layer_one, nullptr);
  ASSERT_NE(layer_two, nullptr);

  auto client_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(client_buf.IsValid());
  ASSERT_TRUE(SetFullscreenClientTarget(client_buf));

  auto buf_one_frame1 = AllocateBuffer(display_width, display_height);
  auto buf_two_frame1 = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(buf_one_frame1.IsValid());
  ASSERT_TRUE(buf_two_frame1.IsValid());
  ASSERT_TRUE(FillRgbaBuffer(buf_one_frame1, display_width, display_height,
                             kSolidBlue));
  ASSERT_TRUE(FillRgbaBuffer(buf_two_frame1, display_width, display_height,
                             kSolidYellow));

  auto writeback_buf_f1 = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf_f1.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf_f1));

  auto imported_one_f1 = ImportLayerBuffer(buf_one_frame1);
  auto imported_two_f1 = ImportLayerBuffer(buf_two_frame1);
  ASSERT_TRUE(imported_one_f1.has_value());
  ASSERT_TRUE(imported_two_f1.has_value());

  HwcLayer::LayerProperties props_one;
  props_one.buffer = imported_one_f1;
  props_one.blend_mode = BufferBlendMode::kNone;
  props_one.colorspace = HwcColorspace::kBt709;
  props_one.sample_range = BufferSampleRange::kFullRange;
  props_one.transfer_func = TransferFunction::kSrgb;
  props_one.composition_type = CompositionType::kDevice;
  props_one.display_frame = FullscreenDisplayFrame();
  props_one.source_crop = FullscreenSourceCrop();
  props_one.z_order = kBaseZOrder;
  props_one.alpha = kAlphaOpaque;
  layer_one->SetLayerProperties(props_one);

  HwcLayer::LayerProperties props_two = props_one;
  props_two.buffer = imported_two_f1;
  props_two.blend_mode = BufferBlendMode::kPreMult;
  props_two.z_order = kBaseZOrder + 1;
  layer_two->SetLayerProperties(props_two);

  auto validate_f1 = primary_display->ValidateStagedComposition();
  EXPECT_TRUE(validate_f1.changed_layers.empty());
  primary_display->AcceptValidatedComposition();

  SharedFd present_fence_f1;
  std::vector<HwcDisplay::ReleaseFence> release_fences_f1;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence_f1,
                                                        release_fences_f1));

  ExpectPixelEq(ReadWritebackPixelFromLastPresent(writeback_buf_f1,
                                                  writeback_format,
                                                  display_width / kHalfDivisor,
                                                  display_height /
                                                      kHalfDivisor),
                kSolidYellow);

  // Frame 2: destroy layer_two and present a new buffer on layer_one.
  EXPECT_TRUE(primary_display->DestroyLayer(kSecondLayerId));
  EXPECT_EQ(primary_display->get_layer(kSecondLayerId), nullptr);

  auto buf_one_frame2 = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(buf_one_frame2.IsValid());
  ASSERT_TRUE(FillRgbaBuffer(buf_one_frame2, display_width, display_height,
                             kSolidCyan));
  auto imported_one_f2 = ImportLayerBuffer(buf_one_frame2);
  ASSERT_TRUE(imported_one_f2.has_value());

  auto writeback_buf_f2 = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf_f2.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf_f2));

  HwcLayer::LayerProperties props_one_f2;
  props_one_f2.buffer = imported_one_f2;
  layer_one->SetLayerProperties(props_one_f2);

  auto validate_f2 = primary_display->ValidateStagedComposition();
  EXPECT_TRUE(validate_f2.changed_layers.empty());
  primary_display->AcceptValidatedComposition();

  SharedFd present_fence_f2;
  std::vector<HwcDisplay::ReleaseFence> release_fences_f2;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence_f2,
                                                        release_fences_f2));

  ExpectPixelEq(ReadWritebackPixelFromLastPresent(writeback_buf_f2,
                                                  writeback_format,
                                                  display_width / kHalfDivisor,
                                                  display_height /
                                                      kHalfDivisor),
                kSolidCyan);
}

// Verifies that display writeback via concurrent writeback captures the
// presented frame into the staged writeback buffer and signals the writeback
// fence.
TEST_F(SdmBackendTest, WritebackCapturesPresentedFrame) {
  std::scoped_lock lock(hwc->GetResMan().GetMainLock());

  const BufferFormat writeback_format = GetPrimaryWritebackFormat();
  ASSERT_NE(writeback_format, BufferFormat::kUndefined);
  const auto hal_format = BufferFormatToHalPixelFormat(writeback_format);
  ASSERT_TRUE(hal_format.has_value());

  ASSERT_TRUE(primary_display->CreateLayer(kFirstLayerId));
  HwcLayer* layer = primary_display->get_layer(kFirstLayerId);
  ASSERT_NE(layer, nullptr);

  auto layer_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(layer_buf.IsValid());
  ASSERT_TRUE(
      FillRgbaBuffer(layer_buf, display_width, display_height, kSolidGreen));

  auto client_buf = AllocateBuffer(display_width, display_height);
  ASSERT_TRUE(client_buf.IsValid());
  ASSERT_TRUE(SetFullscreenClientTarget(client_buf));

  auto writeback_buf = AllocateWritebackBuffer(writeback_format);
  ASSERT_TRUE(writeback_buf.IsValid());
  ASSERT_TRUE(StageWritebackBuffer(writeback_buf));

  auto imported_buf = ImportLayerBuffer(layer_buf);
  ASSERT_TRUE(imported_buf.has_value());

  HwcLayer::LayerProperties props;
  props.buffer = imported_buf;
  props.blend_mode = BufferBlendMode::kNone;
  props.colorspace = HwcColorspace::kBt709;
  props.sample_range = BufferSampleRange::kFullRange;
  props.transfer_func = TransferFunction::kSrgb;
  props.composition_type = CompositionType::kDevice;
  props.display_frame = FullscreenDisplayFrame();
  props.source_crop = FullscreenSourceCrop();
  props.z_order = kBaseZOrder;
  props.alpha = kAlphaOpaque;
  layer->SetLayerProperties(props);

  auto validate_result = primary_display->ValidateStagedComposition();
  EXPECT_TRUE(validate_result.changed_layers.empty());
  primary_display->AcceptValidatedComposition();

  SharedFd present_fence;
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  ASSERT_TRUE(primary_display->PresentStagedComposition(std::nullopt,
                                                        present_fence,
                                                        release_fences));

  ExpectPixelEq(ReadWritebackPixelFromLastPresent(writeback_buf,
                                                  writeback_format,
                                                  display_width / kHalfDivisor,
                                                  display_height /
                                                      kHalfDivisor),
                kSolidGreen);
}

}  // namespace
}  // namespace android::drm_hwcomposer
