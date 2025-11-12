/*
 * Copyright (C) 2024 The Android Open Source Project
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

#define LOG_TAG "drmhwc"
#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)

#include "ComposerClient.h"

#include <cinttypes>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include <aidl/android/hardware/graphics/common/Transform.h>
#include <aidl/android/hardware/graphics/composer3/ClientTarget.h>
#include <aidl/android/hardware/graphics/composer3/Composition.h>
#include <aidl/android/hardware/graphics/composer3/DisplayRequest.h>
#include <aidl/android/hardware/graphics/composer3/IComposerClient.h>
#include <aidl/android/hardware/graphics/composer3/PowerMode.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#include <aidl/android/hardware/graphics/composer3/RenderIntent.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_auto_utils.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/native_handle.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Trace.h>

#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/DisplayInfo.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "hwc3/CommandResultWriter.h"
#include "hwc3/DrmHwcThree.h"
#include "hwc3/Utils.h"
#include "stats/CompositionStatsAtomReporter.h"
#include "stats/CompositionStatsPoller.h"
#include "utils/fd.h"
#include "utils/log.h"
#include "utils/properties.h"

using ::android::drm_hwcomposer::BufferBlendMode;
using ::android::drm_hwcomposer::BufferColorSpace;
using ::android::drm_hwcomposer::BufferSampleRange;
using ::android::drm_hwcomposer::CompositionStatsPoller;
using ::android::drm_hwcomposer::CompositionType;
using ::android::drm_hwcomposer::DamageInfo;
using ::android::drm_hwcomposer::DisplayHandle;
using ::android::drm_hwcomposer::DstRectInfo;
using ::android::drm_hwcomposer::HwcDisplay;
using ::android::drm_hwcomposer::HwcDisplayConfig;
using ::android::drm_hwcomposer::HwcLayer;
using ::android::drm_hwcomposer::IRect;
using ::android::drm_hwcomposer::LayerTransform;
using ::android::drm_hwcomposer::PanelOrientation;
using ::android::drm_hwcomposer::SrcRectInfo;

using HwcOutputType = ::android::drm_hwcomposer::OutputType;
#if __ANDROID_API__ >= 36
using AidlOutputType = aidl::android::hardware::graphics::composer3::OutputType;
#endif

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

std::optional<BufferColorSpace> AidlToColorSpace(
    const common::Dataspace& dataspace) {
  int32_t standard = static_cast<int32_t>(dataspace) &
                     static_cast<int32_t>(common::Dataspace::STANDARD_MASK);
  switch (standard) {
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT709):
      return BufferColorSpace::kItuRec709;
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_625):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_625_UNADJUSTED):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_525):
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT601_525_UNADJUSTED):
      return BufferColorSpace::kItuRec601;
    case static_cast<int32_t>(common::Dataspace::STANDARD_BT2020):
    case static_cast<int32_t>(
        common::Dataspace::STANDARD_BT2020_CONSTANT_LUMINANCE):
      return BufferColorSpace::kItuRec2020;
    case static_cast<int32_t>(common::Dataspace::UNKNOWN):
      return BufferColorSpace::kUndefined;
    default:
      ALOGE("Unsupported standard: %d", standard);
      return std::nullopt;
  }
}

std::optional<BufferColorSpace> AidlToColorSpace(
    const std::optional<ParcelableDataspace>& dataspace) {
  if (!dataspace) {
    return std::nullopt;
  }
  return AidlToColorSpace(dataspace->dataspace);
}

std::optional<BufferSampleRange> AidlToSampleRange(
    const common::Dataspace& dataspace) {
  int32_t sample_range = static_cast<int32_t>(dataspace) &
                         static_cast<int32_t>(common::Dataspace::RANGE_MASK);
  switch (sample_range) {
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

#if __ANDROID_API__ >= 36
AidlOutputType OutputTypeToAidl(const HwcOutputType output_type) {
  switch (output_type) {
    case HwcOutputType::kSystem:
      return AidlOutputType::SYSTEM;
    case HwcOutputType::kSdr:
      return AidlOutputType::SDR;
    case HwcOutputType::kHdr10:
      return AidlOutputType::HDR10;
    case HwcOutputType::kInvalid:
      [[fallthrough]];
    default:
      return AidlOutputType::INVALID;
  }
}
#endif

DisplayConfiguration HwcDisplayConfigToAidlConfiguration(
    int32_t width, int32_t height, const HwcDisplayConfig& config) {
  DisplayConfiguration aidl_configuration =
      {.configId = static_cast<int32_t>(config.id),
       .width = config.mode.GetRawMode().hdisplay,
       .height = config.mode.GetRawMode().vdisplay,
       .configGroup = static_cast<int32_t>(config.group_id),
       .vsyncPeriod = config.mode.GetVSyncPeriodNs()};

#if __ANDROID_API__ >= 36
  aidl_configuration.hdrOutputType = OutputTypeToAidl(config.output_type);
#endif

  if (width > 0) {
    static const float kMmPerInch = 25.4;
    float dpi_x = float(config.mode.GetRawMode().hdisplay) * kMmPerInch /
                float(width);
    float dpi_y = height <= 0 ? dpi_x :
                  float(config.mode.GetRawMode().vdisplay) * kMmPerInch /
                    float(height);
    aidl_configuration.dpi = {.x = dpi_x, .y = dpi_y};
  }
  // TODO: Populate vrrConfig.
  return aidl_configuration;
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

}  // namespace

class Hwc3BufferHandle : public ::android::drm_hwcomposer::PrimeFdsSharedBase {
 public:
  static auto Create(buffer_handle_t handle)
      -> std::shared_ptr<Hwc3BufferHandle> {
    auto hwc3 = std::shared_ptr<Hwc3BufferHandle>(new Hwc3BufferHandle());

    auto result = ::android::GraphicBufferMapper::get()
        .importBufferNoValidate(handle, &hwc3->imported_handle_);

    if (result != ::android::NO_ERROR) {
      ALOGE("Failed to import buffer handle: %d", result);
      return nullptr;
    }

    return hwc3;
  }

  auto GetHandle() const -> buffer_handle_t {
    return imported_handle_;
  }

  ~Hwc3BufferHandle() override {
    ::android::GraphicBufferMapper::get().freeBuffer(imported_handle_);
  }

 private:
  Hwc3BufferHandle() = default;
  buffer_handle_t imported_handle_{};
};

class Hwc3Layer : public ::android::drm_hwcomposer::FrontendLayerBase {
 public:
  auto HandleNextBuffer(std::optional<buffer_handle_t> raw_handle,
                        ::android::drm_hwcomposer::SharedFd fence_fd,
                        int32_t slot_id)
      -> std::optional<HwcLayer::LayerProperties> {
    HwcLayer::LayerProperties lp;
    if (!raw_handle && slots_.count(slot_id) != 0) {
      lp.active_slot = {
          .slot_id = slot_id,
          .fence = std::move(fence_fd),
      };

      return lp;
    }

    if (!raw_handle) {
      ALOGE("Buffer handle is nullopt but slot was not cached.");
      return std::nullopt;
    }

    auto hwc3 = Hwc3BufferHandle::Create(*raw_handle);
    if (!hwc3) {
      return std::nullopt;
    }

    auto bi = ::android::drm_hwcomposer::BufferInfoGetter::GetInstance()
                  ->GetBoInfo(hwc3->GetHandle());
    if (bi) {
      bi->fds_shared = hwc3;

      lp.slot_buffer = {
          .slot_id = slot_id,
          .bi = bi,
      };
    }

    lp.active_slot = {
        .slot_id = slot_id,
        .fence = std::move(fence_fd),
    };

    slots_[slot_id] = hwc3;

    return lp;
  }

  [[maybe_unused]]
  auto HandleClearSlot(int32_t slot_id)
      -> std::optional<HwcLayer::LayerProperties> {
    if (slots_.count(slot_id) == 0) {
      return std::nullopt;
    }

    slots_.erase(slot_id);

    auto lp = HwcLayer::LayerProperties{};
    lp.slot_buffer = {
        .slot_id = slot_id,
        .bi = std::nullopt,
    };

    return lp;
  }

  void ClearSlots() {
    slots_.clear();
  }

 private:
  std::map<int32_t /*slot*/, std::shared_ptr<Hwc3BufferHandle>> slots_;
};

static auto GetHwc3Layer(HwcLayer& layer) -> std::shared_ptr<Hwc3Layer> {
  auto frontend_private_data = layer.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<Hwc3Layer>();
    layer.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<Hwc3Layer>(frontend_private_data);
}

ComposerClient::ComposerClient() {
  DEBUG_FUNC();
}

void ComposerClient::Init() {
  DEBUG_FUNC();
  hwc_ = std::make_unique<DrmHwcThree>();

  auto reporter = ::android::drm_hwcomposer::CompositionStatsAtomReporter::
      Create();
  if (reporter) {
    stats_poller_ = std::make_unique<CompositionStatsPoller>(std::move(
                                                                 reporter),
                                                             hwc_.get());
  }
}

ComposerClient::~ComposerClient() {
  DEBUG_FUNC();
  stats_poller_.reset();
  if (hwc_) {
    const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
    hwc_->DeinitDisplays();
    hwc_.reset();
  }
  ALOGD("removed composer client");
}

ndk::ScopedAStatus ComposerClient::createLayer(int64_t display_handle,
                                               int32_t /*buffer_slot_count*/,
                                               int64_t* layer_id) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());

  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  auto hwc3display = DrmHwcThree::GetHwc3Display(*display);

  if (!display->CreateLayer(hwc3display->next_layer_id)) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  *layer_id = hwc3display->next_layer_id;

  hwc3display->next_layer_id++;

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::createVirtualDisplay(
    int32_t width, int32_t height, AidlPixelFormat format_hint,
    int32_t /*output_buffer_slot_count*/, VirtualDisplay* out_display) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());

  // TODO: Format is currently not used in drm_hwcomposer.
  std::optional<DisplayHandle>
      display_handle = hwc_->CreateVirtualDisplay(width, height);
  if (!display_handle) {
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }

  out_display->display = display_handle.value();
  out_display->format = format_hint;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::destroyLayer(int64_t display_handle,
                                                int64_t layer_id) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  if (!display->DestroyLayer(layer_id)) {
    return ToBinderStatus(hwc3::Error::kBadLayer);
  }

  return ToBinderStatus(hwc3::Error::kNone);
}

ndk::ScopedAStatus ComposerClient::destroyVirtualDisplay(
    int64_t display_handle) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  auto* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }
  if (!hwc_->DestroyVirtualDisplay(display_handle)) {
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }
  return ndk::ScopedAStatus::ok();
}

HwcDisplay* ComposerClient::GetDisplay(int64_t display_handle) {
  return hwc_->GetDisplay(static_cast<DisplayHandle>(display_handle));
}

void ComposerClient::DispatchLayerCommand(int64_t display_handle,
                                          const LayerCommand& command) {
  auto* display = GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer_->AddError(hwc3::Error::kBadDisplay);
    return;
  }

  auto batch_command = command.layerLifecycleBatchCommandType;
  if (batch_command == LayerLifecycleBatchCommandType::CREATE) {
    if (!display->CreateLayer(command.layer)) {
      cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
      return;
    }
  }

  if (batch_command == LayerLifecycleBatchCommandType::DESTROY) {
    if (!display->DestroyLayer(command.layer)) {
      cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
    }

    return;
  }

  auto* layer = display->get_layer(command.layer);
  if (layer == nullptr) {
    cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
    return;
  }

  if (command.luts) {
    ALOGI("setLayerLuts unsupported: display=%lld layer=%lld",
          (long long)display_handle, (long long)command.layer);
    cmd_result_writer_->AddError(hwc3::Error::kUnsupported);
    return;
  }

  // If the requested composition type is not supported, the HWC should return
  // an error and not process any further commands.
  if (!IsSupportedCompositionType(command.composition)) {
    cmd_result_writer_->AddError(hwc3::Error::kUnsupported);
    return;
  }

  // For some invalid parameters, the HWC should return an error and not process
  // any further commands.
  if (!ValidateLayerBrightness(command.brightness)) {
    cmd_result_writer_->AddError(hwc3::Error::kBadParameter);
    return;
  }

  /* https://source.android.com/docs/core/graphics/reduce-consumption */
  if (command.bufferSlotsToClear) {
    auto hwc3_layer = GetHwc3Layer(*layer);
    for (const auto& slot : *command.bufferSlotsToClear) {
      auto lp = hwc3_layer->HandleClearSlot(slot);
      if (!lp) {
        cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
        return;
      }

      layer->SetLayerProperties(lp.value());
    }
  }

  HwcLayer::LayerProperties properties;
  if (command.buffer) {
    auto hwc3_layer = GetHwc3Layer(*layer);
    std::optional<buffer_handle_t> buffer_handle = std::nullopt;
    if (command.buffer->handle) {
      buffer_handle = ::android::makeFromAidl(*command.buffer->handle);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto fence = const_cast<::ndk::ScopedFileDescriptor&>(command.buffer->fence)
                     .release();

    auto lp = hwc3_layer
                  ->HandleNextBuffer(buffer_handle,
                                     ::android::drm_hwcomposer::MakeSharedFd(
                                         fence),
                                     command.buffer->slot);

    if (!lp) {
      cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
      return;
    }

    properties = lp.value();
  }

  properties.blend_mode = AidlToBlendMode(command.blendMode);
  properties.color_space = AidlToColorSpace(command.dataspace);
  properties.sample_range = AidlToSampleRange(command.dataspace);
  properties.composition_type = AidlToCompositionType(command.composition);
  properties.display_frame = AidlToDstRect(command.displayFrame);
  properties.alpha = AidlToAlpha(command.planeAlpha);
  properties.source_crop = AidlToSrcRect(command.sourceCrop);
  properties.transform = AidlToLayerTransform(command.transform);
  properties.z_order = AidlToZOrder(command.z);
  properties.damage = AidlToDamage(command.damage);

  layer->SetLayerProperties(properties);

  // Some unsupported functionality returns kUnsupported, and others
  // are just a no-op.
  // TODO: Audit whether some of these should actually return kUnsupported
  // instead.
  if (command.sidebandStream) {
    cmd_result_writer_->AddError(hwc3::Error::kUnsupported);
  }
  // TODO: Blocking region handling missing.
  // TODO: Layer visible region.
  // TODO: Per-frame metadata.
  // TODO: Layer color transform.
  // TODO: Layer cursor position.
  // TODO: Layer color.
}

void ComposerClient::ExecuteDisplayCommand(const DisplayCommand& command) {
  ATRACE_CALL();

  const int64_t display_handle = command.display;
  HwcDisplay* display = hwc_->GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer_->AddError(hwc3::Error::kBadDisplay);
    return;
  }

  if (command.brightness) {
    // TODO: Implement support for display brightness.
    cmd_result_writer_->AddError(hwc3::Error::kUnsupported);
    return;
  }

  hwc3::Error error = ValidateColorTransformMatrix(
      command.colorTransformMatrix);
  if (error != hwc3::Error::kNone) {
    ALOGE("Invalid color transform matrix.");
    cmd_result_writer_->AddError(error);
    return;
  }

  for (const auto& layer_cmd : command.layers) {
    DispatchLayerCommand(command.display, layer_cmd);
  }

  if (cmd_result_writer_->HasError()) {
    return;
  }

  if (command.clientTarget) {
    ExecuteSetDisplayClientTarget(command.display, *command.clientTarget);
  }
  if (command.virtualDisplayOutputBuffer) {
    ExecuteSetDisplayOutputBuffer(command.display,
                                  *command.virtualDisplayOutputBuffer);
  }

  std::optional<std::array<float, kCtmSize>> ctm = AidlToColorTransformMatrix(
      command.colorTransformMatrix);
  if (ctm) {
    display->SetColorTransformMatrix(ctm.value());
  }

  bool shall_present_now = false;

  DisplayChanges changes{};
  if (command.validateDisplay || command.presentOrValidateDisplay) {
    std::vector<HwcDisplay::ChangedLayer>
        changed_layers = display->ValidateStagedComposition();
    for (auto [layer_id, composition_type] : changed_layers) {
      changes.AddLayerCompositionChange(command.display, layer_id,
                                        static_cast<Composition>(
                                            composition_type));
    }
    cmd_result_writer_->AddChanges(changes);
    auto hwc3_display = DrmHwcThree::GetHwc3Display(*display);
    hwc_->ClearMustValidateDisplay(display_handle);
    hwc3_display->desired_present_time = AidlToPresentTimeNs(
        command.expectedPresentTime);

    // TODO: DisplayRequests are not implemented.
  }

  if (command.presentOrValidateDisplay) {
    auto result = PresentOrValidate::Result::Validated;
    if (!display->NeedsClientLayerUpdate() && !changes.HasAnyChanges()) {
      ALOGV("Skipping SF roundtrip for display %" PRId64, display_handle);
      result = PresentOrValidate::Result::Presented;
      shall_present_now = true;
    }
    cmd_result_writer_->AddPresentOrValidateResult(display_handle, result);
  }

  if (command.acceptDisplayChanges) {
    display->AcceptValidatedComposition();
  }

  if (command.presentDisplay || shall_present_now) {
    auto hwc3_display = DrmHwcThree::GetHwc3Display(*display);
    if (hwc_->GetMustValidateDisplay(display_handle)) {
      cmd_result_writer_->AddError(hwc3::Error::kNotValidated);
      return;
    }

    ::android::drm_hwcomposer::SharedFd present_fence;
    std::vector<HwcDisplay::ReleaseFence> release_fences;
    bool ret = display->PresentStagedComposition(hwc3_display
                                                     ->desired_present_time,
                                                 present_fence, release_fences);

    if (!ret) {
      cmd_result_writer_->AddError(hwc3::Error::kBadDisplay);
      return;
    }

    using ::android::base::unique_fd;
    cmd_result_writer_->AddPresentFence(  //
        display_handle,
        unique_fd(::android::drm_hwcomposer::DupFd(present_fence)));

    std::unordered_map<int64_t, unique_fd> hal_release_fences;
    for (const auto& [layer_id, release_fence] : release_fences) {
      hal_release_fences[layer_id] = unique_fd(
          ::android::drm_hwcomposer::DupFd(release_fence));
    }
    cmd_result_writer_->AddReleaseFence(display_handle, hal_release_fences);
  }
}

ndk::ScopedAStatus ComposerClient::executeCommands(
    const std::vector<DisplayCommand>& commands,
    std::vector<CommandResultPayload>* results) {
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  DEBUG_FUNC();
  cmd_result_writer_ = std::make_unique<CommandResultWriter>(results);
  for (const auto& cmd : commands) {
    ExecuteDisplayCommand(cmd);
    cmd_result_writer_->IncrementCommand();
  }
  cmd_result_writer_.reset();

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getActiveConfig(int64_t display_handle,
                                                   int32_t* config_id) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  const HwcDisplayConfig* config = display->GetLastRequestedConfig();
  if (config == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadConfig);
  }

  *config_id = config->id;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getColorModes(
    int64_t display_handle, std::vector<AidlColorMode>* color_modes) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  for (const auto& mode : display->GetColorModes()) {
    color_modes->emplace_back(static_cast<AidlColorMode>(mode));
  }

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDataspaceSaturationMatrix(
    common::Dataspace dataspace, std::vector<float>* matrix) {
  DEBUG_FUNC();
  if (dataspace != common::Dataspace::SRGB_LINEAR) {
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  matrix->clear();
  matrix->insert(matrix->begin(),
                 ::android::drm_hwcomposer::kIdentityMatrix.begin(),
                 ::android::drm_hwcomposer::kIdentityMatrix.end());

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayAttribute(
    int64_t display_handle, int32_t config_id, DisplayAttribute attribute,
    int32_t* value) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  const auto* config = display->GetConfig(config_id);
  if (config == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadConfig);
  }

  const auto bounds = display->GetDisplayBoundsMm();
  DisplayConfiguration aidl_configuration = HwcDisplayConfigToAidlConfiguration(
      /*width =*/bounds.first,
      /*height =*/bounds.second, *config);
  // Legacy API for querying DPI uses units of dots per 1000 inches.
  static const int kLegacyDpiUnit = 1000;
  switch (attribute) {
    case DisplayAttribute::WIDTH:
      *value = aidl_configuration.width;
      break;
    case DisplayAttribute::HEIGHT:
      *value = aidl_configuration.height;
      break;
    case DisplayAttribute::VSYNC_PERIOD:
      *value = aidl_configuration.vsyncPeriod;
      break;
    case DisplayAttribute::DPI_X:
      if (!aidl_configuration.dpi)
        return ToBinderStatus(hwc3::Error::kUnsupported);
      *value = static_cast<int>(aidl_configuration.dpi->x * kLegacyDpiUnit);
      break;
    case DisplayAttribute::DPI_Y:
      if (!aidl_configuration.dpi)
        return ToBinderStatus(hwc3::Error::kUnsupported);
      *value = static_cast<int>(aidl_configuration.dpi->y * kLegacyDpiUnit);
      break;
    case DisplayAttribute::CONFIG_GROUP:
      *value = aidl_configuration.configGroup;
      break;
    case DisplayAttribute::INVALID:
      return ToBinderStatus(hwc3::Error::kUnsupported);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayCapabilities(
    int64_t display_handle, std::vector<DisplayCapability>* caps) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  if (GetDisplay(display_handle) == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // Skip color transform altogether if device/drm cannot support it.
  if (hwc_->GetResMan().GetCtmHandling() ==
      ::android::drm_hwcomposer::CtmHandling::kDrmOrIgnore) {
    caps->emplace_back(DisplayCapability::SKIP_CLIENT_COLOR_TRANSFORM);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigs(
    int64_t display_handle, std::vector<int32_t>* out_configs) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  for (const auto& config : display->GetDisplayConfigs()) {
    out_configs->push_back(config.id);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConnectionType(
    int64_t display_handle, DisplayConnectionType* type) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  switch (display->GetDisplayType()) {
    case HwcDisplay::DisplayType::kVirtual:
      return ToBinderStatus(hwc3::Error::kBadDisplay);
    case HwcDisplay::DisplayType::kInternal:
      *type = DisplayConnectionType::INTERNAL;
      break;
    case HwcDisplay::DisplayType::kExternal:
      *type = DisplayConnectionType::EXTERNAL;
      break;
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayIdentificationData(
    int64_t display_handle, DisplayIdentification* id) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  id->port = static_cast<int8_t>(display->GetPort());
  id->data = display->GetRawEdid();
  if (id->data.empty()) {
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayName(int64_t display_handle,
                                                  std::string* name) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  *name = display->GetDisplayName();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayVsyncPeriod(
    int64_t display_handle, int32_t* vsync_period) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // getDisplayVsyncPeriod should return the vsync period of the config that
  // is currently committed to the kernel. If a config change is pending due to
  // setActiveConfigWithConstraints, return the pre-change vsync period.
  const HwcDisplayConfig* config = display->GetCurrentConfig();
  if (config == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadConfig);
  }

  *vsync_period = config->mode.GetVSyncPeriodNs();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayedContentSample(
    int64_t /*display_handle*/, int64_t /*max_frames*/, int64_t /*timestamp*/,
    DisplayContentSample* /*samples*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getDisplayedContentSamplingAttributes(
    int64_t /*display_handle*/, DisplayContentSamplingAttributes* /*attrs*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getDisplayPhysicalOrientation(
    int64_t display_handle, common::Transform* orientation) {
  DEBUG_FUNC();

  if (orientation == nullptr) {
    ALOGE("Invalid 'orientation' pointer.");
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  PanelOrientation
      drm_orientation = display->getDisplayPhysicalOrientation().value_or(
          PanelOrientation::kModePanelOrientationNormal);

  switch (drm_orientation) {
    case PanelOrientation::kModePanelOrientationNormal:
      *orientation = common::Transform::NONE;
      break;
    case PanelOrientation::kModePanelOrientationBottomUp:
      *orientation = common::Transform::ROT_180;
      break;
    case PanelOrientation::kModePanelOrientationLeftUp:
      *orientation = common::Transform::ROT_270;
      break;
    case PanelOrientation::kModePanelOrientationRightUp:
      *orientation = common::Transform::ROT_90;
      break;
    default:
      ALOGE("Unknown panel orientation value: %d",
            static_cast<int>(drm_orientation));
      return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getHdrCapabilities(int64_t display_handle,
                                                      HdrCapabilities* caps) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  display->GetHdrCapabilities(&caps->types, &caps->maxLuminance,
                              &caps->maxAverageLuminance, &caps->minLuminance);

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getMaxVirtualDisplayCount(int32_t* count) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  *count = static_cast<int32_t>(hwc_->GetMaxVirtualDisplayCount());
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getPerFrameMetadataKeys(
    int64_t /*display_handle*/, std::vector<PerFrameMetadataKey>* /*keys*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getReadbackBufferAttributes(
    int64_t display_handle, ReadbackBufferAttributes* attrs) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());

  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  if (!display->IsWritebackSupported()) {
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }

  // TODO(markyacoub): Query the writeback connector to determine the supported
  // readback buffer attributes (format, dataspace, etc.) Currently, default
  // values are used.
  attrs->format = common::PixelFormat::RGBA_8888;
  attrs->dataspace = common::Dataspace::SRGB;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getReadbackBufferFence(
    int64_t display_handle, ndk::ScopedFileDescriptor* acquire_fence) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());

  *acquire_fence = ndk::ScopedFileDescriptor(-1);

  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // Check if this display supports readback operations
  if (!display->IsWritebackSupported()) {
    ALOGI("ComposerClient: Display %" PRId64 " does not support readback",
          display_handle);
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }

  ::android::drm_hwcomposer::SharedFd fence = display
                                                  ->GetWritebackBufferFence();
  display->SetWritebackEnabled(false);
  display->GetWritebackLayer()->ClearSlots();

  if (!fence) {
    ALOGE("ComposerClient: Failed to get readback buffer fence");
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }

  if (fence && *fence >= 0) {
    *acquire_fence = ndk::ScopedFileDescriptor(
        ::android::drm_hwcomposer::DupFd(fence));
  }

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getRenderIntents(
    int64_t display_handle, AidlColorMode mode,
    std::vector<RenderIntent>* intents) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // TODO: Remove invalid enum tests from VTS
  if (mode < AidlColorMode::NATIVE || mode > AidlColorMode::DISPLAY_BT2020)
    return ToBinderStatus(hwc3::Error::kBadParameter);

  intents->clear();
  intents->reserve(1);
  intents->emplace_back(RenderIntent::COLORIMETRIC);

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getSupportedContentTypes(
    int64_t display_handle, std::vector<ContentType>* types) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // Support for ContentType is not implemented.
  types->clear();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayDecorationSupport(
    int64_t /*display_handle*/,
    std::optional<common::DisplayDecorationSupport>* /*support_struct*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::registerCallback(
    const std::shared_ptr<IComposerCallback>& callback) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  // This function is specified to be called exactly once.
  hwc_->Init(callback);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setActiveConfig(int64_t display_handle,
                                                   int32_t config) {
  DEBUG_FUNC();

  VsyncPeriodChangeTimeline timeline;
  VsyncPeriodChangeConstraints constraints = {
      .desiredTimeNanos = ::android::drm_hwcomposer::ResourceManager::
          GetTimeMonotonicNs(),
      .seamlessRequired = false,
  };
  return setActiveConfigWithConstraints(display_handle, config, constraints,
                                        &timeline);
}

ndk::ScopedAStatus ComposerClient::setActiveConfigWithConstraints(
    int64_t display_handle, int32_t config,
    const VsyncPeriodChangeConstraints& constraints,
    VsyncPeriodChangeTimeline* timeline) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  if (constraints.seamlessRequired) {
    return ToBinderStatus(hwc3::Error::kSeamlessNotAllowed);
  }

  const HwcDisplayConfig* current_config = display->GetCurrentConfig();
  const HwcDisplayConfig* next_config = display->GetConfig(config);
  const bool same_resolution = current_config != nullptr &&
                               next_config != nullptr &&
                               current_config->mode.SameSize(next_config->mode);

  /* Client framebuffer management:
   * https://source.android.com/docs/core/graphics/framebuffer-mgmt
   */
  if (!same_resolution) {
    auto& client_layer = display->GetClientLayer();
    auto hwc3_layer = GetHwc3Layer(client_layer);
    hwc3_layer->ClearSlots();
    client_layer.ClearSlots();
  }

  // Always try to queue a seamless commit to reduce jank and flicker artifacts.
  // Fall-back to a full blocking commit otherwise.
  ::android::drm_hwcomposer::QueuedConfigTiming timing{};
  auto error = display->QueueConfig(config, constraints.desiredTimeNanos,
                                    &timing);
  if (error == HwcDisplay::kNone) {
    timeline->newVsyncAppliedTimeNanos = timing.new_vsync_time_ns;
    timeline->refreshTimeNanos = timing.refresh_time_ns;
    timeline->refreshRequired = true;
  } else if (error == HwcDisplay::kSeamlessNotAllowed) {
    ALOGE_IF(constraints.seamlessRequired,
             "Seamless modeset not possible with requested config=%d. Falling "
             "back to a blocking full modeset.",
             config);

    error = display->SetConfig(config);
    timeline->newVsyncAppliedTimeNanos = ::android::drm_hwcomposer::
        ResourceManager::GetTimeMonotonicNs();
    timeline->refreshRequired = false;
  }

  switch (error) {
    case HwcDisplay::ConfigError::kBadConfig:
      return ToBinderStatus(hwc3::Error::kBadConfig);
    case HwcDisplay::ConfigError::kSeamlessNotAllowed:
      return ToBinderStatus(hwc3::Error::kSeamlessNotAllowed);
    case HwcDisplay::ConfigError::kSeamlessNotPossible:
      return ToBinderStatus(hwc3::Error::kSeamlessNotPossible);
#if __ANDROID_API__ >= 36
    case HwcDisplay::ConfigError::kConfigFailed:
      return ToBinderStatus(hwc3::Error::kConfigFailed);
#else
    case HwcDisplay::ConfigError::kConfigFailed:
      return ToBinderStatus(hwc3::Error::kBadConfig);
    #endif
    case HwcDisplay::ConfigError::kNone:
      hwc_->LogRefreshRateChanges();
      return ndk::ScopedAStatus::ok();
  }
}

ndk::ScopedAStatus ComposerClient::setBootDisplayConfig(
    int64_t /*display_handle*/, int32_t /*config*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::clearBootDisplayConfig(
    int64_t /*display_handle*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getPreferredBootDisplayConfig(
    int64_t /*display_handle*/, int32_t* /*config*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setAutoLowLatencyMode(int64_t display_handle,
                                                         bool /*on*/) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setClientTargetSlotCount(
    int64_t /*display_handle*/, int32_t /*count*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kNone);
}

ndk::ScopedAStatus ComposerClient::setColorMode(int64_t display_handle,
                                                AidlColorMode mode,
                                                RenderIntent intent) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // TODO: Remove invalid enum tests from VTS
  if (mode < AidlColorMode::NATIVE || mode > AidlColorMode::DISPLAY_BT2020)
    return ToBinderStatus(hwc3::Error::kBadParameter);

  if (intent < RenderIntent::COLORIMETRIC || intent > RenderIntent::TONE_MAP_ENHANCE)
    return ToBinderStatus(hwc3::Error::kBadParameter);

  if (intent != RenderIntent::COLORIMETRIC)
    return ToBinderStatus(hwc3::Error::kUnsupported);

  display->SetColorMode(
      static_cast<::android::drm_hwcomposer::ColorMode>(mode));
  return ToBinderStatus(hwc3::Error::kNone);
}

ndk::ScopedAStatus ComposerClient::setContentType(int64_t display_handle,
                                                  ContentType type) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  if (type == ContentType::NONE) {
    return ndk::ScopedAStatus::ok();
  }
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setDisplayedContentSamplingEnabled(
    int64_t /*display_handle*/, bool /*enable*/,
    FormatColorComponent /*componentMask*/, int64_t /*maxFrames*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setPowerMode(int64_t display_handle,
                                                PowerMode mode) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // Only OFF and ON are supported. VTS requires checking for invalid enum
  // values.
  switch (static_cast<int32_t>(mode)) {
    case static_cast<int32_t>(PowerMode::OFF):
    case static_cast<int32_t>(PowerMode::ON):
      break;
    case static_cast<int32_t>(PowerMode::DOZE):
    case static_cast<int32_t>(PowerMode::DOZE_SUSPEND):
    case static_cast<int32_t>(PowerMode::ON_SUSPEND):
      return ToBinderStatus(hwc3::Error::kUnsupported);
    default:
      return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  if (!display->SetDisplayEnabled(mode == PowerMode::ON)) {
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setReadbackBuffer(
    int64_t display_handle, const AidlNativeHandle& aidl_buffer,
    const ndk::ScopedFileDescriptor& release_fence_in) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());

  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  if (!display->IsWritebackSupported()) {
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }

  if (!display->SetWritebackEnabled(true)) {
    ALOGE("ComposerClient: Failed to enable writeback");
    return ToBinderStatus(hwc3::Error::kUnsupported);
  }

  buffer_handle_t raw_buffer = ::android::makeFromAidl(aidl_buffer);
  if (raw_buffer == nullptr) {
    ALOGE("ComposerClient: Failed to convert AIDL handle to buffer_handle_t");
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  buffer_handle_t imported_handle = nullptr;
  auto result = ::android::GraphicBufferMapper::get()
                    .importBufferNoValidate(raw_buffer, &imported_handle);
  if (result != ::android::OK) {
    ALOGE("ComposerClient: Failed to import readback buffer handle: %d",
          result);
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }
  HwcLayer::LayerProperties properties;
  properties.slot_buffer = {
      .slot_id = 0,
      .bi = ::android::drm_hwcomposer::BufferInfoGetter::GetInstance()
                ->GetBoInfo(imported_handle),
  };
  ndk::ScopedFileDescriptor release_fence = ndk::ScopedFileDescriptor(
      release_fence_in.get());
  properties.active_slot = {
      .slot_id = 0,
      .fence = ::android::drm_hwcomposer::MakeSharedFd(release_fence.release()),
  };
  properties.blend_mode = BufferBlendMode::kNone;

  std::unique_ptr<HwcLayer>& writeback_layer = display->GetWritebackLayer();
  if (!writeback_layer) {
    ALOGE("HwcDisplay: Writeback layer not available");
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }
  writeback_layer->SetLayerProperties(properties);

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setVsyncEnabled(int64_t display_handle,
                                                   bool enabled) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  display->SetVsyncCallbacksEnabled(enabled);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setIdleTimerEnabled(
    int64_t /*display_handle*/, int32_t /*timeout*/) {
  DEBUG_FUNC();
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getOverlaySupport(
    OverlayProperties* /*out_overlay_properties*/) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getHdrConversionCapabilities(
    std::vector<common::HdrConversionCapability>* /*out_capabilities*/) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setHdrConversionStrategy(
    const common::HdrConversionStrategy& /*conversion_strategy*/,
    common::Hdr* /*out_hdr*/) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setRefreshRateChangedCallbackDebugEnabled(
    int64_t /*display*/, bool /*enabled*/) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigurations(
    int64_t display_handle, int32_t /*max_frame_interval_ns*/,
    std::vector<DisplayConfiguration>* configurations) {
  DEBUG_FUNC();
  const std::unique_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  const auto bounds = display->GetDisplayBoundsMm();
  for (const auto& config : display->GetDisplayConfigs()) {
    configurations->push_back(
        HwcDisplayConfigToAidlConfiguration(/*width =*/bounds.first,
                                            /*height =*/bounds.second, config));
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::notifyExpectedPresent(
    int64_t /*display*/,
    const ClockMonotonicTimestamp& /*expected_present_time*/,
    int32_t /*frame_interval_ns*/) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

#if __ANDROID_API__ >= 36

ndk::ScopedAStatus ComposerClient::startHdcpNegotiation(
    int64_t display_handle, const drm::HdcpLevels& levels) {
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }
  // Client can only request lazy HDCP activation/start
  // TODO: Add HDCP terminate/stop request once client handles it
  if (levels.connectedLevel != drm::HdcpLevel::HDCP_NONE &&
      levels.connectedLevel != drm::HdcpLevel::HDCP_UNKNOWN) {
    ALOGI("Requested to start HDCP for connected level : %d",
          static_cast<int>(levels.connectedLevel));
    if (!display->StartHdcp(true)) {
      return ToBinderStatus(hwc3::Error::kUnsupported);
    }
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getMaxLayerPictureProfiles(int64_t /* display */,
                                                              int32_t* /* max_profiles */) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

ndk::ScopedAStatus ComposerClient::getLuts(int64_t /* display */,
                                           const std::vector<Buffer>& /* buffers */,
                                           std::vector<Luts>* /* out_luts */) {
  return ToBinderStatus(hwc3::Error::kUnsupported);
}

#endif

std::string ComposerClient::Dump() {
  return hwc_->DumpState();
}

::ndk::SpAIBinder ComposerClient::createBinder() {
  auto binder = BnComposerClient::createBinder();
  AIBinder_setInheritRt(binder.get(), true);
  return binder;
}

void ComposerClient::ExecuteSetDisplayClientTarget(
    int64_t display_handle, const ClientTarget& command) {
  auto* display = GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer_->AddError(hwc3::Error::kBadDisplay);
    return;
  }

  auto& client_layer = display->GetClientLayer();
  auto hwc3layer = GetHwc3Layer(client_layer);

  std::optional<buffer_handle_t> raw_buffer = std::nullopt;
  if (command.buffer.handle) {
    raw_buffer = ::android::makeFromAidl(*command.buffer.handle);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto fence = const_cast<::ndk::ScopedFileDescriptor&>(command.buffer.fence)
                   .release();

  auto properties = hwc3layer->HandleNextBuffer(raw_buffer,
                                                ::android::drm_hwcomposer::
                                                    MakeSharedFd(fence),
                                                command.buffer.slot);

  if (!properties) {
    ALOGE("Failed to import client target buffer.");
    /* Here, sending an error would be the natural way to do the thing.
     * But VTS checks for no error. Is it the VTS issue?
     * https://cs.android.com/android/platform/superproject/main/+/main:hardware/interfaces/graphics/composer/aidl/vts/VtsHalGraphicsComposer3_TargetTest.cpp;l=1892;drc=2647200f4c535ca6567b452695b7d13f2aaf3f2a
     */
    return;
  }

  properties->color_space = AidlToColorSpace(command.dataspace);
  properties->sample_range = AidlToSampleRange(command.dataspace);

  client_layer.SetLayerProperties(properties.value());
}

void ComposerClient::ExecuteSetDisplayOutputBuffer(int64_t display_handle,
                                                   const Buffer& buffer) {
  auto* display = GetDisplay(display_handle);
  if (display == nullptr) {
    cmd_result_writer_->AddError(hwc3::Error::kBadDisplay);
    return;
  }

  auto& writeback_layer = display->GetWritebackLayer();
  if (!writeback_layer) {
    cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
    return;
  }

  auto hwc3layer = GetHwc3Layer(*writeback_layer);

  std::optional<buffer_handle_t> raw_buffer = std::nullopt;
  if (buffer.handle) {
    raw_buffer = ::android::makeFromAidl(*buffer.handle);
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto fence = const_cast<::ndk::ScopedFileDescriptor&>(buffer.fence).release();

  auto properties = hwc3layer->HandleNextBuffer(raw_buffer,
                                                ::android::drm_hwcomposer::
                                                    MakeSharedFd(fence),
                                                buffer.slot);

  if (!properties) {
    cmd_result_writer_->AddError(hwc3::Error::kBadLayer);
    return;
  }

  writeback_layer->SetLayerProperties(properties.value());
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
