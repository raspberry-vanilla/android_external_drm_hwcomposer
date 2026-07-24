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

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)

#include "ComposerClient.h"

#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/HdrConversionStrategy.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidl/android/hardware/graphics/common/Transform.h>
#include <aidl/android/hardware/graphics/composer3/BnComposerClient.h>
#include <aidl/android/hardware/graphics/composer3/ClientTarget.h>
#include <aidl/android/hardware/graphics/composer3/CommandResultPayload.h>
#include <aidl/android/hardware/graphics/composer3/ContentType.h>
#include <aidl/android/hardware/graphics/composer3/DisplayAttribute.h>
#include <aidl/android/hardware/graphics/composer3/DisplayCapability.h>
#include <aidl/android/hardware/graphics/composer3/DisplayConnectionType.h>
#include <aidl/android/hardware/graphics/composer3/FormatColorComponent.h>
#include <aidl/android/hardware/graphics/composer3/IComposerClient.h>
#include <aidl/android/hardware/graphics/composer3/LayerCommand.h>
#include <aidl/android/hardware/graphics/composer3/PerFrameMetadataKey.h>
#include <aidl/android/hardware/graphics/composer3/PowerMode.h>
#include <aidl/android/hardware/graphics/composer3/RenderIntent.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_auto_utils.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/native_handle.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Errors.h>

#if __ANDROID_API__ >= 36
#include <aidl/android/hardware/drm/HdcpLevel.h>
#include <aidl/android/hardware/graphics/composer3/OutputType.h>
#endif

#include <cinttypes>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "backend/BackendManager.h"
#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/GrallocBufferCache.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "drm/DrmDevice.h"
#include "drm/DrmFbImporter.h"
#include "hwc/HwcDisplay.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "hwc3/CommandResultWriter.h"
#include "hwc3/ComposerClientUtils.h"
#include "hwc3/DrmHwcThree.h"
#include "hwc3/Utils.h"
#include "stats/CompositionStatsAtomReporter.h"
#include "stats/CountActiveDisplaysReporter.h"
#include "stats/StatsPoller.h"
#include "utils/fd.h"
#include "utils/log.h"
#include "utils/properties.h"

using ::android::drm_hwcomposer::BufferBlendMode;
using ::android::drm_hwcomposer::DisplayHandle;
using ::android::drm_hwcomposer::GrallocBufferCache;
using ::android::drm_hwcomposer::HwcDisplay;
using ::android::drm_hwcomposer::HwcDisplayConfig;
using ::android::drm_hwcomposer::HwcLayer;
using ::android::drm_hwcomposer::IDrmFbIdHandle;
using ::android::drm_hwcomposer::IRect;
using ::android::drm_hwcomposer::PanelOrientation;
using ::android::drm_hwcomposer::StatsPoller;

#if __ANDROID_API__ >= 36
using HwcOutputType = ::android::drm_hwcomposer::OutputType;
using AidlOutputType = aidl::android::hardware::graphics::composer3::OutputType;
#endif

namespace aidl::android::hardware::graphics::composer3::impl {
namespace {

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

}  // namespace

ComposerClient::ComposerClient() {
  DEBUG_FUNC();
}

void ComposerClient::Init() {
  DEBUG_FUNC();
  hwc_ = std::make_unique<DrmHwcThree>();

  auto composition_reporter = ::android::drm_hwcomposer::
      CompositionStatsAtomReporter::Create();
  auto count_active_displays_reporter = ::android::drm_hwcomposer::
      CountActiveDisplaysReporter::Create();
  if (composition_reporter && count_active_displays_reporter) {
    stats_poller_ = std::make_unique<
        StatsPoller>(std::move(composition_reporter),
                     std::move(count_active_displays_reporter), hwc_.get());
  }
}

ComposerClient::~ComposerClient() {
  DEBUG_FUNC();
  stats_poller_.reset();
  if (hwc_) {
    {
      std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
      hwc_->DeinitDisplays();
    }
    hwc_.reset();
  }
  ALOGD("removed composer client");
}

ndk::ScopedAStatus ComposerClient::createLayer(int64_t display_handle,
                                               int32_t /*buffer_slot_count*/,
                                               int64_t* layer_id) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

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
  // TODO: Format is currently not used in drm_hwcomposer.

  std::optional<DisplayHandle> display_handle = std::nullopt;
  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
    display_handle = hwc_->CreateVirtualDisplay(width, height);
  }
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
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

ndk::ScopedAStatus ComposerClient::executeCommands(
    const std::vector<DisplayCommand>& commands,
    std::vector<CommandResultPayload>* results) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  CommandResultWriter cmd_result_writer(results);
  for (const auto& cmd : commands) {
    ExecuteDisplayCommand(*hwc_, cmd, cmd_result_writer);
    cmd_result_writer.IncrementCommand();
  }

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getActiveConfig(int64_t display_handle,
                                                   int32_t* config_id) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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

  // Note: IILE
  const auto configuration_get_result =
      [display_handle, config_id,
       this]() -> std::variant<hwc3::Error, DisplayConfiguration> {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

    const HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
      return hwc3::Error::kBadDisplay;

    const auto* config = display->GetConfig(config_id);
    if (config == nullptr)
      return hwc3::Error::kBadConfig;

    const auto bounds = display->GetDisplayBoundsMm();
    return HwcDisplayConfigToAidlConfiguration(/*width =*/bounds.first,
                                               /*height =*/bounds.second,
                                               *config);
  }();

  if (const auto* error = std::get_if<hwc3::Error>(&configuration_get_result)) {
    return ToBinderStatus(*error);
  }
  const auto& aidl_configuration = std::get<DisplayConfiguration>(
      configuration_get_result);

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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  // Skip color transform altogether if device/drm cannot support it.
  if (hwc_->GetResMan().GetCtmHandling() ==
      ::android::drm_hwcomposer::CtmHandling::kDrmOrIgnore) {
    caps->emplace_back(DisplayCapability::SKIP_CLIENT_COLOR_TRANSFORM);
  }
  if (display->HasBacklight()) {
    caps->emplace_back(DisplayCapability::BRIGHTNESS);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigs(
    int64_t display_handle, std::vector<int32_t>* out_configs) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  *name = display->GetDisplayName();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayVsyncPeriod(
    int64_t display_handle, int32_t* vsync_period) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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

  // Note: IILE
  const auto orientation_get_result =
      [display_handle, this]() -> std::variant<hwc3::Error, PanelOrientation> {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

    const HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
      return hwc3::Error::kBadDisplay;

    return display->getDisplayPhysicalOrientation().value_or(
        PanelOrientation::kModePanelOrientationNormal);
  }();

  if (const auto* error = std::get_if<hwc3::Error>(&orientation_get_result)) {
    return ToBinderStatus(*error);
  }
  const PanelOrientation drm_orientation = std::get<PanelOrientation>(
      orientation_get_result);

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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  display->GetHdrCapabilities(&caps->types, &caps->maxLuminance,
                              &caps->maxAverageLuminance, &caps->minLuminance);

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getMaxVirtualDisplayCount(int32_t* count) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
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
  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

    const HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
      return ToBinderStatus(hwc3::Error::kBadDisplay);

    if (!display->IsWritebackSupported())
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

  *acquire_fence = ndk::ScopedFileDescriptor(-1);

  std::unique_lock lock(hwc_->GetResMan().GetMainLock());

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
  // TODO: Remove invalid enum tests from VTS
  if (mode < AidlColorMode::NATIVE || mode > AidlColorMode::DISPLAY_BT2020)
    return ToBinderStatus(hwc3::Error::kBadParameter);

  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
    const HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
      return ToBinderStatus(hwc3::Error::kBadDisplay);
  }

  intents->clear();
  intents->reserve(1);
  intents->emplace_back(RenderIntent::COLORIMETRIC);

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getSupportedContentTypes(
    int64_t display_handle, std::vector<ContentType>* types) {
  DEBUG_FUNC();
  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
    const HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
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

  if (constraints.seamlessRequired) {
    return ToBinderStatus(hwc3::Error::kSeamlessNotAllowed);
  }

  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  HwcDisplay* display = GetDisplay(display_handle);
  if (display == nullptr) {
    return ToBinderStatus(hwc3::Error::kBadDisplay);
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
    std::shared_ptr<GrallocBufferCache>
        buffer_cache = GetBufferCache(display, client_layer);
    buffer_cache->ClearSlots();
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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
  const HwcDisplay* display = GetDisplay(display_handle);
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

  // TODO: Remove invalid enum tests from VTS
  if (mode < AidlColorMode::NATIVE || mode > AidlColorMode::DISPLAY_BT2020)
    return ToBinderStatus(hwc3::Error::kBadParameter);

  if (intent < RenderIntent::COLORIMETRIC || intent > RenderIntent::TONE_MAP_ENHANCE)
    return ToBinderStatus(hwc3::Error::kBadParameter);

  if (intent != RenderIntent::COLORIMETRIC)
    return ToBinderStatus(hwc3::Error::kUnsupported);

  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

    HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
      return ToBinderStatus(hwc3::Error::kBadDisplay);

    display->SetColorMode(
        static_cast<::android::drm_hwcomposer::ColorMode>(mode));
  }
  return ToBinderStatus(hwc3::Error::kNone);
}

ndk::ScopedAStatus ComposerClient::setContentType(int64_t display_handle,
                                                  ContentType type) {
  DEBUG_FUNC();
  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());
    const HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
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

static constexpr hwc3::Error DisplayToAidlError(HwcDisplay::Error err) {
  switch (err) {
    case HwcDisplay::Error::kNone:
      return hwc3::Error::kNone;
    case HwcDisplay::Error::kBadParameter:
      return hwc3::Error::kBadParameter;
    case HwcDisplay::Error::kUnsupported:
      return hwc3::Error::kUnsupported;
  }
}

ndk::ScopedAStatus ComposerClient::setPowerMode(int64_t display_handle,
                                                PowerMode mode) {
  DEBUG_FUNC();

  // Only OFF and ON are supported. VTS requires checking for invalid enum
  // values.
  HwcDisplay::PowerMode hwc_mode = HwcDisplay::PowerMode::kOn;
  switch (mode) {
    case PowerMode::OFF:
      hwc_mode = HwcDisplay::PowerMode::kOff;
      break;
    case PowerMode::ON:
      hwc_mode = HwcDisplay::PowerMode::kOn;
      break;
    case PowerMode::DOZE:
      hwc_mode = HwcDisplay::PowerMode::kDoze;
      break;
    case PowerMode::DOZE_SUSPEND:
      hwc_mode = HwcDisplay::PowerMode::kDozeSuspend;
      break;
    case PowerMode::ON_SUSPEND:
      hwc_mode = HwcDisplay::PowerMode::kSuspend;
      break;
    default:
      return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  {
    std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

    HwcDisplay* display = GetDisplay(display_handle);
    if (display == nullptr)
      return ToBinderStatus(hwc3::Error::kBadDisplay);

    auto err = display->SetPowerMode(hwc_mode);
    return ToBinderStatus(DisplayToAidlError(err));
  }
}

ndk::ScopedAStatus ComposerClient::setReadbackBuffer(
    int64_t display_handle, const AidlNativeHandle& aidl_buffer,
    const ndk::ScopedFileDescriptor& release_fence_in) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

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

  std::unique_ptr<const native_handle_t, NativeHandleDeleter> raw_buffer(
      ::android::makeFromAidl(aidl_buffer));
  if (raw_buffer == nullptr) {
    ALOGE("ComposerClient: Failed to convert AIDL handle to buffer_handle_t");
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  buffer_handle_t imported_handle = nullptr;
  auto result = ::android::GraphicBufferMapper::get()
                    .importBufferNoValidate(raw_buffer.get(), &imported_handle);

  if (result != ::android::OK) {
    ALOGE("ComposerClient: Failed to import readback buffer handle: %d",
          result);
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }

  std::unique_ptr<HwcLayer>& writeback_layer = display->GetWritebackLayer();
  if (!writeback_layer) {
    ALOGE("HwcDisplay: Writeback layer not available");
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }
  HwcLayer::LayerProperties properties;
  ndk::ScopedFileDescriptor release_fence = ndk::ScopedFileDescriptor(
      release_fence_in.get());
  properties.blend_mode = BufferBlendMode::kNone;
  auto bi = ::android::drm_hwcomposer::BufferInfoGetter::GetInstance()
                ->GetBoInfo(imported_handle);
  if (bi == std::nullopt) {
    ALOGE("Failed to get BufferInfo for readback buffer.");
    return ToBinderStatus(hwc3::Error::kBadParameter);
  }
  properties.buffer = {
      .bi = bi.value(),
      .fb = std::static_pointer_cast<IDrmFbIdHandle>(ImportFb(display, *bi)),
      .fence = ::android::drm_hwcomposer::MakeSharedFd(release_fence.release()),
  };
  writeback_layer->SetLayerProperties(properties);

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setVsyncEnabled(int64_t display_handle,
                                                   bool enabled) {
  DEBUG_FUNC();
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

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
  std::scoped_lock lock(hwc_->GetResMan().GetMainLock());

  const HwcDisplay* display = GetDisplay(display_handle);
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
    if (!display->StartHdcp()) {
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
  std::stringstream output;
  output << hwc_->DumpState();
  output << "\n- Backends\n";
  auto dump = hwc_->GetResMan().DumpBackends();
  output << dump.value_or("N/A\n");
  return output.str();
}

::ndk::SpAIBinder ComposerClient::createBinder() {
  auto binder = BnComposerClient::createBinder();
  AIBinder_setInheritRt(binder.get(), true);
  return binder;
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
