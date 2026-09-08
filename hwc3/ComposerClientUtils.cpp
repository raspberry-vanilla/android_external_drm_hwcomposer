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

#include "ComposerClientUtils.h"

#include <aidl/android/hardware/graphics/common/BlendMode.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/FRect.h>
#include <aidl/android/hardware/graphics/common/Transform.h>
#include <aidl/android/hardware/graphics/composer3/Composition.h>
#include <aidl/android/hardware/graphics/composer3/DisplayBrightness.h>
#include <aidl/android/hardware/graphics/composer3/DisplayCommand.h>
#include <aidl/android/hardware/graphics/composer3/DisplayLuts.h>
#include <aidl/android/hardware/graphics/composer3/LayerBrightness.h>
#include <aidl/android/hardware/graphics/composer3/LayerCommand.h>
#include <aidl/android/hardware/graphics/composer3/LayerLifecycleBatchCommandType.h>
#include <aidl/android/hardware/graphics/composer3/LutProperties.h>
#include <aidl/android/hardware/graphics/composer3/Luts.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableBlendMode.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableComposition.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableDataspace.h>
#include <aidl/android/hardware/graphics/composer3/ParcelableTransform.h>
#include <aidl/android/hardware/graphics/composer3/PlaneAlpha.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#include <aidl/android/hardware/graphics/composer3/ZOrder.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/unique_fd.h>
#include <cutils/ashmem.h>
#include <cutils/native_handle.h>
#include <sys/mman.h>  // IWYU pragma: keep
#include <utils/Trace.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/GrallocBufferCache.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmHwc.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcLayer.h"
#include "hwc3/CommandResultWriter.h"
#include "hwc3/DrmHwcThree.h"
#include "hwc3/Utils.h"
#include "utils/ColorUtil.h"
#include "utils/fd.h"
#include "utils/log.h"

using ::android::drm_hwcomposer::BufferBlendMode;
using ::android::drm_hwcomposer::BufferColorEncoding;
using ::android::drm_hwcomposer::BufferInfo;
using ::android::drm_hwcomposer::BufferSampleRange;
using ::android::drm_hwcomposer::ColorUtil;
using ::android::drm_hwcomposer::CompositionType;
using ::android::drm_hwcomposer::DamageInfo;
using ::android::drm_hwcomposer::DrmFbIdHandle;
using ::android::drm_hwcomposer::DrmHwc;
using ::android::drm_hwcomposer::DstRectInfo;
using ::android::drm_hwcomposer::DupFd;
using ::android::drm_hwcomposer::GrallocBufferCache;
using ::android::drm_hwcomposer::HwcColorspace;
using ::android::drm_hwcomposer::HwcDisplay;
using ::android::drm_hwcomposer::HwcLayer;
using ::android::drm_hwcomposer::IRect;
using ::android::drm_hwcomposer::LayerTransform;
using ::android::drm_hwcomposer::MakeSharedFd;
using ::android::drm_hwcomposer::SharedFd;
using ::android::drm_hwcomposer::SrcRectInfo;
using ::android::drm_hwcomposer::TransferFunction;

namespace aidl::android::hardware::graphics::composer3::impl {

namespace {

constexpr int kCtmRows = 4;
constexpr int kCtmColumns = 4;
constexpr int kCtmSize = kCtmRows * kCtmColumns;

std::optional<BufferBlendMode> AidlToBlendMode(
    const std::optional<ParcelableBlendMode>& aidl_blend_mode) {
  if (!aidl_blend_mode) {
    return std::nullopt;
  }

  switch (aidl_blend_mode->blendMode) {
    case common::BlendMode::NONE:
      return BufferBlendMode::kNone;
    case common::BlendMode::PREMULTIPLIED:
      return BufferBlendMode::kPreMult;
    case common::BlendMode::COVERAGE:
      return BufferBlendMode::kCoverage;
    case common::BlendMode::INVALID:
      ALOGE("Invalid BlendMode");
      return std::nullopt;
  }
}

std::optional<HwcColorspace> AidlToColorspace(
    const common::Dataspace& dataspace) {
  int32_t standard = static_cast<int32_t>(dataspace) &
                     static_cast<int32_t>(common::Dataspace::STANDARD_MASK);
  switch (standard) {
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT709):
      return HwcColorspace::kBt709;
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_625):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_625_UNADJUSTED):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_525):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_525_UNADJUSTED):
      return HwcColorspace::kBt601;
    case static_cast<int32_t>(common::Dataspace::STANDARD_DCI_P3):
      return HwcColorspace::kDciP3;
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT2020):
    case static_cast<int32_t>(
        common::Dataspace::STANDARD_BT2020_CONSTANT_LUMINANCE):
      return HwcColorspace::kBt2020;
    case static_cast<int32_t>(common::Dataspace::UNKNOWN):
      return HwcColorspace::kDefault;
    default:
      ALOGE("Unsupported standard: %d", standard);
      return std::nullopt;
  }
}

std::optional<HwcColorspace> AidlToColorspace(
    const std::optional<ParcelableDataspace>& dataspace) {
  if (!dataspace) {
    return std::nullopt;
  }
  return AidlToColorspace(dataspace->dataspace);
}

std::optional<BufferColorEncoding> AidlToColorEncoding(
    const common::Dataspace& dataspace) {
  int32_t standard = static_cast<int32_t>(dataspace) &
                     static_cast<int32_t>(common::Dataspace::STANDARD_MASK);
  switch (standard) {
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT709):
      return BufferColorEncoding::kItuRec709;
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_625):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_625_UNADJUSTED):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_525):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_525_UNADJUSTED):
      return BufferColorEncoding::kItuRec601;
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT2020):
    case static_cast<int32_t>(
        common::Dataspace::STANDARD_BT2020_CONSTANT_LUMINANCE):
      return BufferColorEncoding::kItuRec2020;
    case static_cast<int32_t>(common::Dataspace::STANDARD_DCI_P3):
    case static_cast<int32_t>(common::Dataspace::UNKNOWN):
      return BufferColorEncoding::kUndefined;
    default:
      ALOGE("Unsupported standard: %d", standard);
      return std::nullopt;
  }
}

std::optional<BufferColorEncoding> AidlToColorEncoding(
    const std::optional<ParcelableDataspace>& dataspace) {
  if (!dataspace) {
    return std::nullopt;
  }
  return AidlToColorEncoding(dataspace->dataspace);
}

std::optional<BufferSampleRange> AidlToSampleRange(
    const common::Dataspace& dataspace) {
  int32_t sample_range = static_cast<int32_t>(dataspace) &
                         static_cast<int32_t>(common::Dataspace::RANGE_MASK);
  switch (sample_range) {
    case static_cast<int32_t>(common::Dataspace::RANGE_EXTENDED):
      // Extended implies full + headroom
      [[fallthrough]];
    case static_cast<int32_t>(common::Dataspace::RANGE_FULL):
      return BufferSampleRange::kFullRange;
    case static_cast<int32_t>(common::Dataspace::RANGE_LIMITED):
      return BufferSampleRange::kLimitedRange;
    case static_cast<int32_t>(common::Dataspace::UNKNOWN):
      return BufferSampleRange::kUndefined;
    default:
      ALOGE("Unsupported sample range: %d", sample_range);
      return std::nullopt;
  }
}

std::optional<BufferSampleRange> AidlToSampleRange(
    const std::optional<ParcelableDataspace>& dataspace) {
  if (!dataspace) {
    return std::nullopt;
  }
  return AidlToSampleRange(dataspace->dataspace);
}

std::optional<TransferFunction> AidlToTransferFunc(
    const common::Dataspace& dataspace) {
  int32_t transfer_func = static_cast<int32_t>(dataspace) &
                          static_cast<int32_t>(
                              common::Dataspace::TRANSFER_MASK);
  switch (transfer_func) {
    case static_cast<int32_t>(common::Dataspace::TRANSFER_ST2084):
      return TransferFunction::kPq;
    case static_cast<int32_t>(common::Dataspace::TRANSFER_HLG):
      return TransferFunction::kHlg;
    case static_cast<int32_t>(common::Dataspace::TRANSFER_SRGB):
      return TransferFunction::kSrgb;
    case static_cast<int32_t>(common::Dataspace::TRANSFER_SMPTE_170M):
      return TransferFunction::kSmpte170M;
    case static_cast<int32_t>(common::Dataspace::TRANSFER_UNSPECIFIED):
      return TransferFunction::kUnknown;
    default:
      ALOGE("Unsupported transfer function: %d", transfer_func);
      return std::nullopt;
  }
}

std::optional<TransferFunction> AidlToTransferFunc(
    const std::optional<ParcelableDataspace>& dataspace) {
  if (!dataspace) {
    return std::nullopt;
  }
  return AidlToTransferFunc(dataspace->dataspace);
}

std::optional<int64_t> AidlToPresentTimeNs(
    const std::optional<ClockMonotonicTimestamp>& expected_present_time) {
  if (!expected_present_time || expected_present_time->timestampNanos == 0) {
    return std::nullopt;
  }
  return expected_present_time->timestampNanos;
}

bool IsSupportedCompositionType(
    const std::optional<ParcelableComposition> composition) {
  if (!composition) {
    return true;
  }
  switch (composition->composition) {
    case Composition::INVALID:
    case Composition::CLIENT:
    case Composition::DEVICE:
    case Composition::SOLID_COLOR:
    case Composition::CURSOR:
      return true;

    // Unsupported composition types. Set an error for the current
    // DisplayCommand and return.
    case Composition::DISPLAY_DECORATION:
    case Composition::SIDEBAND:
    case Composition::REFRESH_RATE_INDICATOR:
      return false;
  }
}

hwc3::Error ValidateColorTransformMatrix(
    const std::optional<std::vector<float>>& color_transform_matrix) {
  if (!color_transform_matrix) {
    return hwc3::Error::kNone;
  }

  if (color_transform_matrix->size() != kCtmSize) {
    ALOGE("Expected color transform matrix of size %d, got size %d.", kCtmSize,
          (int)color_transform_matrix->size());
    return hwc3::Error::kBadParameter;
  }

  return hwc3::Error::kNone;
}

bool ValidateLayerBrightness(const std::optional<LayerBrightness>& brightness) {
  if (!brightness) {
    return true;
  }
  return !(std::signbit(brightness->brightness) ||
           std::isnan(brightness->brightness));
}

std::optional<std::array<float, kCtmSize>> AidlToColorTransformMatrix(
    const std::optional<std::vector<float>>& aidl_color_transform_matrix) {
  if (!aidl_color_transform_matrix ||
      aidl_color_transform_matrix->size() < kCtmSize) {
    return std::nullopt;
  }

  std::array<float, kCtmSize>
      color_transform_matrix = ::android::drm_hwcomposer::kIdentityMatrix;
  std::copy(aidl_color_transform_matrix->begin(),
            aidl_color_transform_matrix->end(), color_transform_matrix.begin());
  return color_transform_matrix;
}

std::optional<CompositionType> AidlToCompositionType(
    const std::optional<ParcelableComposition> composition) {
  if (!composition) {
    return std::nullopt;
  }

  switch (composition->composition) {
    case Composition::INVALID:
      return CompositionType::kInvalid;
    case Composition::CLIENT:
      return CompositionType::kClient;
    case Composition::DEVICE:
      return CompositionType::kDevice;
    case Composition::SOLID_COLOR:
      return CompositionType::kSolidColor;
    case Composition::CURSOR:
      return CompositionType::kCursor;

    // Unsupported composition types.
    case Composition::DISPLAY_DECORATION:
    case Composition::SIDEBAND:
    case Composition::REFRESH_RATE_INDICATOR:
      ALOGE("Unsupported composition type: %s",
            toString(composition->composition).c_str());
      return std::nullopt;
  }
}

std::optional<IRect> AidlToIRect(const std::optional<common::Rect>& rect) {
  if (!rect) {
    return std::nullopt;
  }
  return IRect{.left = rect->left,
               .top = rect->top,
               .right = rect->right,
               .bottom = rect->bottom};
}

std::optional<DstRectInfo> AidlToDstRect(
    const std::optional<common::Rect>& rect) {
  auto i_rect = AidlToIRect(rect);
  if (!i_rect) {
    return std::nullopt;
  }
  return DstRectInfo{.i_rect = i_rect};
}

std::optional<SrcRectInfo> AidlToSrcRect(
    const std::optional<common::FRect>& rect) {
  if (!rect) {
    return std::nullopt;
  }
  SrcRectInfo src_rect;
  src_rect.f_rect = {.left = rect->left,
                     .top = rect->top,
                     .right = rect->right,
                     .bottom = rect->bottom};
  return src_rect;
}

std::optional<float> AidlToAlpha(const std::optional<PlaneAlpha>& alpha) {
  if (!alpha) {
    return std::nullopt;
  }
  return alpha->alpha;
}

std::optional<uint32_t> AidlToZOrder(const std::optional<ZOrder>& z_order) {
  if (!z_order) {
    return std::nullopt;
  }
  return z_order->z;
}

std::optional<LayerTransform> AidlToLayerTransform(
    const std::optional<ParcelableTransform>& aidl_transform) {
  if (!aidl_transform) {
    return std::nullopt;
  }

  using aidl::android::hardware::graphics::common::Transform;

  return (LayerTransform){
      .hflip = (int32_t(aidl_transform->transform) &
                int32_t(Transform::FLIP_H)) != 0,
      .vflip = (int32_t(aidl_transform->transform) &
                int32_t(Transform::FLIP_V)) != 0,
      .rotate90 = (int32_t(aidl_transform->transform) &
                   int32_t(Transform::ROT_90)) != 0,
  };
}

std::optional<DamageInfo> AidlToDamage(
    const std::optional<std::vector<std::optional<common::Rect>>>& damage) {
  if (!damage.has_value()) {
    return std::nullopt;
  }

  DamageInfo damage_info;
  for (const auto& r : damage.value()) {
    auto i_rect = AidlToIRect(r);
    if (i_rect.has_value()) {
      damage_info.dmg_rects.push_back(i_rect.value());
    }
  }

  return std::make_optional(damage_info);
}

// Layer commands

void DispatchLayerCommand(DrmHwc& hwc, int64_t display_handle,
                          const LayerCommand& command,
                          CommandResultWriter& cmd_result_writer) {
  auto* display = hwc.GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer.AddError(hwc3::Error::kBadDisplay);
    return;
  }

  auto batch_command = command.layerLifecycleBatchCommandType;
  if (batch_command == LayerLifecycleBatchCommandType::CREATE) {
    if (!display->CreateLayer(command.layer)) {
      cmd_result_writer.AddError(hwc3::Error::kBadLayer);
      return;
    }
  }

  if (batch_command == LayerLifecycleBatchCommandType::DESTROY) {
    if (!display->DestroyLayer(command.layer)) {
      cmd_result_writer.AddError(hwc3::Error::kBadLayer);
    }

    return;
  }

  auto* layer = display->get_layer(command.layer);
  if (layer == nullptr) {
    cmd_result_writer.AddError(hwc3::Error::kBadLayer);
    return;
  }

  if (command.luts) {
    ALOGI("setLayerLuts unsupported: display=%lld layer=%lld",
          (long long)display_handle, (long long)command.layer);
    cmd_result_writer.AddError(hwc3::Error::kUnsupported);
    return;
  }

  // If the requested composition type is not supported, the HWC should return
  // an error and not process any further commands.
  if (!IsSupportedCompositionType(command.composition)) {
    cmd_result_writer.AddError(hwc3::Error::kUnsupported);
    return;
  }

  // For some invalid parameters, the HWC should return an error and not process
  // any further commands.
  if (!ValidateLayerBrightness(command.brightness)) {
    cmd_result_writer.AddError(hwc3::Error::kBadParameter);
    return;
  }

  /* https://source.android.com/docs/core/graphics/reduce-consumption */
  if (command.bufferSlotsToClear) {
    auto buffer_cache = GetBufferCache(display, *layer);
    for (const auto& slot : *command.bufferSlotsToClear) {
      buffer_cache->ClearSlot(slot);
    }
  }

  HwcLayer::LayerProperties properties;
  if (command.buffer) {
    auto buffer_cache = GetBufferCache(display, *layer);
    std::unique_ptr<const native_handle_t, NativeHandleDeleter> buffer_handle;
    if (command.buffer->handle) {
      buffer_handle.reset(::android::makeFromAidl(*command.buffer->handle));
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto fence = const_cast<::ndk::ScopedFileDescriptor&>(command.buffer->fence)
                     .release();

    auto lp = buffer_cache->HandleNextBuffer(buffer_handle
                                                 ? std::make_optional(
                                                       buffer_handle.get())
                                                 : std::nullopt,
                                             MakeSharedFd(fence),
                                             command.buffer->slot);

    if (!lp) {
      cmd_result_writer.AddError(hwc3::Error::kBadLayer);
      return;
    }

    properties.buffer = lp;
  }

  properties.blend_mode = AidlToBlendMode(command.blendMode);
  properties.colorspace = AidlToColorspace(command.dataspace);
  properties.color_encoding = AidlToColorEncoding(command.dataspace);
  properties.sample_range = AidlToSampleRange(command.dataspace);
  properties.transfer_func = AidlToTransferFunc(command.dataspace);
  properties.composition_type = AidlToCompositionType(command.composition);
  properties.display_frame = AidlToDstRect(command.displayFrame);
  properties.alpha = AidlToAlpha(command.planeAlpha);
  properties.source_crop = AidlToSrcRect(command.sourceCrop);
  properties.transform = AidlToLayerTransform(command.transform);
  properties.z_order = AidlToZOrder(command.z);
  properties.damage = AidlToDamage(command.damage);
  properties.brightness = command.brightness.has_value()
                              ? std::make_optional(
                                    command.brightness->brightness)
                              : std::nullopt;

  layer->SetLayerProperties(properties);

  // Some unsupported functionality returns kUnsupported, and others
  // are just a no-op.
  // TODO: Audit whether some of these should actually return kUnsupported
  // instead.
  if (command.sidebandStream) {
    cmd_result_writer.AddError(hwc3::Error::kUnsupported);
  }
  // TODO: Blocking region handling missing.
  // TODO: Layer visible region.
  // TODO: Per-frame metadata.
  // TODO: Layer color transform.
  // TODO: Layer cursor position.
  // TODO: Layer color.
}

// Display commands

void ExecuteSetDisplayBrightness(DrmHwc& hwc, int64_t display_handle,
                                 const DisplayBrightness& brightness,
                                 CommandResultWriter& cmd_result_writer) {
  auto* display = hwc.GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer.AddError(hwc3::Error::kBadDisplay);
    return;
  }

  if (!display->HasBacklight()) {
    cmd_result_writer.AddError(hwc3::Error::kUnsupported);
    return;
  }

  if (!display->SetBrightness(brightness.brightness)) {
    cmd_result_writer.AddError(hwc3::Error::kBadParameter);
  }
}

void ExecuteSetDisplayClientTarget(DrmHwc& hwc, int64_t display_handle,
                                   const ClientTarget& command,
                                   CommandResultWriter& cmd_result_writer) {
  auto* display = hwc.GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer.AddError(hwc3::Error::kBadDisplay);
    return;
  }

  auto& client_layer = display->GetClientLayer();
  auto buffer_cache = GetBufferCache(display, client_layer);

  std::unique_ptr<const native_handle_t, NativeHandleDeleter> raw_buffer;
  if (command.buffer.handle) {
    raw_buffer.reset(::android::makeFromAidl(*command.buffer.handle));
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto fence = const_cast<::ndk::ScopedFileDescriptor&>(command.buffer.fence)
                   .release();

  auto buffer = buffer_cache->HandleNextBuffer(raw_buffer
                                                   ? std::make_optional(
                                                         raw_buffer.get())
                                                   : std::nullopt,
                                               MakeSharedFd(fence),
                                               command.buffer.slot);

  if (!buffer) {
    ALOGE("Failed to import client target buffer.");
    /* Here, sending an error would be the natural way to do the thing.
     * But VTS checks for no error. Is it the VTS issue?
     * https://cs.android.com/android/platform/superproject/main/+/main:hardware/interfaces/graphics/composer/aidl/vts/VtsHalGraphicsComposer3_TargetTest.cpp;l=1892;drc=2647200f4c535ca6567b452695b7d13f2aaf3f2a
     */
    return;
  }
  HwcLayer::LayerProperties properties = {
      .buffer = buffer,
      .color_encoding = AidlToColorEncoding(command.dataspace),
      .sample_range = AidlToSampleRange(command.dataspace),
      .colorspace = AidlToColorspace(command.dataspace),
      .transfer_func = AidlToTransferFunc(command.dataspace),
  };
  client_layer.SetLayerProperties(properties);

  cmd_result_writer.AddClientTarget(display_handle, command.dataspace,
                                    ToPixelFormat(
                                        display->GetWritebackBufferFormat()));
}

void ExecuteSetDisplayOutputBuffer(DrmHwc& hwc, int64_t display_handle,
                                   const Buffer& buffer,
                                   CommandResultWriter& cmd_result_writer) {
  auto* display = hwc.GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer.AddError(hwc3::Error::kBadDisplay);
    return;
  }

  auto& writeback_layer = display->GetWritebackLayer();
  if (!writeback_layer) {
    cmd_result_writer.AddError(hwc3::Error::kBadLayer);
    return;
  }

  auto buffer_cache = GetBufferCache(display, *writeback_layer);

  std::unique_ptr<const native_handle_t, NativeHandleDeleter> raw_buffer;
  if (buffer.handle) {
    raw_buffer.reset(::android::makeFromAidl(*buffer.handle));
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto fence = const_cast<::ndk::ScopedFileDescriptor&>(buffer.fence).release();

  HwcLayer::LayerProperties properties = {
      .buffer = buffer_cache->HandleNextBuffer(raw_buffer
                                                   ? std::make_optional(
                                                         raw_buffer.get())
                                                   : std::nullopt,
                                               MakeSharedFd(fence),
                                               buffer.slot),
  };

  if (!properties.buffer) {
    cmd_result_writer.AddError(hwc3::Error::kBadLayer);
    return;
  }

  writeback_layer->SetLayerProperties(properties);
}

const SharedFd& GetCachedHdrBoostLutFd() {
  static const SharedFd kLutFd = []() -> SharedFd {
    constexpr size_t kBufferSize = ColorUtil::kHdrBoostLut.size() *
                                   sizeof(float);
    ::android::base::unique_fd fd(
        ashmem_create_region("hwc_layer_lut", kBufferSize));
    if (fd < 0) {
      ALOGE("Failed to create ashmem region for hwc_layer_lut: %d", fd.get());
      return nullptr;
    }
    // NOLINTNEXTLINE(misc-include-cleaner)
    void* ptr = mmap(nullptr, kBufferSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd.get(), 0);
    if (ptr == MAP_FAILED) {
      ALOGE("Failed to mmap ashmem for hwc_layer_lut");
      return nullptr;
    }
    std::memcpy(ptr, ColorUtil::kHdrBoostLut.data(), kBufferSize);
    munmap(ptr, kBufferSize);
    // NOLINTNEXTLINE(misc-include-cleaner)
    ashmem_set_prot_region(fd.get(), PROT_READ);
    return MakeSharedFd(fd.release());
  }();
  return kLutFd;
}

}  // namespace

void NativeHandleDeleter::operator()(const native_handle_t* h) const {
  if (h != nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    native_handle_delete(const_cast<native_handle_t*>(h));
  }
}

auto ImportFb(const HwcDisplay* display, BufferInfo& bi)
    -> std::shared_ptr<DrmFbIdHandle> {
  if (display->IsInHeadlessMode()) {
    return nullptr;
  }
  return display->GetPipe().importer->GetOrCreateFbId(&bi);
}

auto GetBufferCache(HwcDisplay* parent, HwcLayer& layer)
    -> std::shared_ptr<GrallocBufferCache> {
  auto frontend_private_data = layer.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<GrallocBufferCache>(
        [parent](auto& bi) -> std::shared_ptr<DrmFbIdHandle> {
          return ImportFb(parent, bi);
        });
    layer.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<GrallocBufferCache>(frontend_private_data);
}

void ExecuteDisplayCommand(DrmHwcThree& hwc, const DisplayCommand& command,
                           CommandResultWriter& cmd_result_writer) {
  ATRACE_CALL();

  const int64_t display_handle = command.display;
  HwcDisplay* display = hwc.GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer.AddError(hwc3::Error::kBadDisplay);
    return;
  }

  hwc3::Error error = ValidateColorTransformMatrix(
      command.colorTransformMatrix);
  if (error != hwc3::Error::kNone) {
    ALOGE("Invalid color transform matrix.");
    cmd_result_writer.AddError(error);
    return;
  }

  for (const auto& layer_cmd : command.layers) {
    DispatchLayerCommand(hwc, command.display, layer_cmd, cmd_result_writer);
  }
  if (cmd_result_writer.HasError()) {
    return;
  }

  if (command.brightness) {
    ExecuteSetDisplayBrightness(hwc, command.display, *command.brightness,
                                cmd_result_writer);
  }
  if (command.clientTarget) {
    ExecuteSetDisplayClientTarget(hwc, command.display, *command.clientTarget,
                                  cmd_result_writer);
  }
  if (command.virtualDisplayOutputBuffer) {
    ExecuteSetDisplayOutputBuffer(hwc, command.display,
                                  *command.virtualDisplayOutputBuffer,
                                  cmd_result_writer);
  }

  std::optional<std::array<float, kCtmSize>> ctm = AidlToColorTransformMatrix(
      command.colorTransformMatrix);
  if (ctm) {
    display->SetColorTransformMatrix(ctm.value());
  }

  bool shall_present_now = false;

  DisplayChanges changes{};
  if (command.validateDisplay || command.presentOrValidateDisplay) {
    auto [changed_layers,
          punch_out_layers] = display->ValidateStagedComposition();
    for (auto [layer_id, composition_type] : changed_layers) {
      // Occluded layers are exposed as client composited to
      // SurfaceFlinger, but dropped by drmhwc before committing.
      if (composition_type == CompositionType::kDeviceOccluded) {
        composition_type = CompositionType::kClient;
      }

      changes.AddLayerCompositionChange(command.display, layer_id,
                                        static_cast<Composition>(
                                            composition_type));
    }
    for (auto layer_id : punch_out_layers) {
      changes.AddLayerClearRequest(command.display, layer_id);
    }
    cmd_result_writer.AddChanges(changes);

    auto buffer_format = display->GetWritebackBufferFormat();
    if (buffer_format == ::android::drm_hwcomposer::BufferFormat::kRgbaFp16) {
      std::vector<DisplayLuts::LayerLut> layer_luts;
      constexpr int32_t kLutSize = ColorUtil::kHdrBoostLut.size();
      const SharedFd& lut_fd = GetCachedHdrBoostLutFd();
      if (lut_fd) {
        for (auto& [layer_id, layer] : display->layers()) {
          if (layer.GetLayerData().transfer_func == TransferFunction::kPq) {
            int dup_fd = DupFd(lut_fd);
            if (dup_fd >= 0) {
              Luts layer_lut;
              layer_lut.pfd = ndk::ScopedFileDescriptor(dup_fd);
              layer_lut.offsets = std::vector<int32_t>{0};
              layer_lut.lutProperties = {
                  LutProperties{
                      .dimension = LutProperties::Dimension::ONE_D,
                      .size = kLutSize,
                      .samplingKeys = {LutProperties::SamplingKey::MAX_RGB},
                  },
              };
              layer_luts.push_back(DisplayLuts::LayerLut{
                  .layer = layer_id,
                  .luts = std::move(layer_lut),
              });
            }
          }
        }

        if (!layer_luts.empty()) {
          cmd_result_writer.AddDisplayLuts(display_handle,
                                           std::move(layer_luts));
        }
      }
    }

    auto hwc3_display = DrmHwcThree::GetHwc3Display(*display);
    hwc.ClearMustValidateDisplay(display_handle);
    hwc3_display->desired_present_time = AidlToPresentTimeNs(
        command.expectedPresentTime);
  }

  if (command.presentOrValidateDisplay) {
    auto result = PresentOrValidate::Result::Validated;
    if (!display->NeedsClientLayerUpdate() && !changes.HasAnyChanges()) {
      ALOGV("Skipping SF roundtrip for display %" PRId64, display_handle);
      result = PresentOrValidate::Result::Presented;
      shall_present_now = true;
    }
    cmd_result_writer.AddPresentOrValidateResult(display_handle, result);
  }

  if (command.acceptDisplayChanges) {
    display->AcceptValidatedComposition();
  }

  if (command.presentDisplay || shall_present_now) {
    auto hwc3_display = DrmHwcThree::GetHwc3Display(*display);
    if (hwc.GetMustValidateDisplay(display_handle)) {
      cmd_result_writer.AddError(hwc3::Error::kNotValidated);
      return;
    }

    ::android::drm_hwcomposer::SharedFd present_fence;
    std::vector<HwcDisplay::ReleaseFence> release_fences;
    bool ret = display->PresentStagedComposition(hwc3_display
                                                     ->desired_present_time,
                                                 present_fence, release_fences);

    if (!ret) {
      cmd_result_writer.AddError(hwc3::Error::kBadDisplay);
      return;
    }

    using ::android::base::unique_fd;
    cmd_result_writer.AddPresentFence(display_handle,
                                      unique_fd(DupFd(present_fence)));

    std::unordered_map<int64_t, unique_fd> hal_release_fences;
    for (const auto& [layer_id, release_fence] : release_fences) {
      hal_release_fences[layer_id] = unique_fd(DupFd(release_fence));
    }
    cmd_result_writer.AddReleaseFence(display_handle, hal_release_fences);
  }
}

auto ToPixelFormat(::android::drm_hwcomposer::BufferFormat format)
    -> common::PixelFormat {
  switch (format) {
    case ::android::drm_hwcomposer::BufferFormat::kRgba8888:
      return common::PixelFormat::RGBA_8888;
    case ::android::drm_hwcomposer::BufferFormat::kXrgb8888:
      return common::PixelFormat::RGBX_8888;
    case ::android::drm_hwcomposer::BufferFormat::kRgbaFp16:
      return common::PixelFormat::RGBA_FP16;
    case ::android::drm_hwcomposer::BufferFormat::kRgba1010102:
      return common::PixelFormat::RGBA_1010102;
    case ::android::drm_hwcomposer::BufferFormat::kUndefined:
    default:
      ALOGW("Unsupported buffer format: %u, using RGBA_8888 instead.",
            static_cast<uint32_t>(format));
      return common::PixelFormat::RGBA_8888;
  }
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
