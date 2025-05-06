/*
 * Copyright (C) 2022 The Android Open Source Project
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

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
// #define LOG_NDEBUG 0 // Uncomment to see HWC2 API calls in logcat

#define LOG_TAG "drmhwc"

#include <cassert>
#include <cinttypes>
#include <memory>
#include <optional>

#include <cutils/native_handle.h>

#include "DrmHwcTwo.h"
#include "backend/Backend.h"
#include "hwc2_device/HwcLayer.h"
#include "utils/log.h"

namespace android {

static int32_t ConfigErrorToHWC2(HwcDisplay::ConfigError result) {
  switch (result) {
    case HwcDisplay::ConfigError::kBadConfig:
      return static_cast<int32_t>(HWC2::Error::BadConfig);
    case HwcDisplay::ConfigError::kSeamlessNotAllowed:
      return static_cast<int32_t>(HWC2::Error::SeamlessNotAllowed);
    case HwcDisplay::ConfigError::kSeamlessNotPossible:
      return static_cast<int32_t>(HWC2::Error::SeamlessNotPossible);
    case HwcDisplay::ConfigError::kNone:
      return static_cast<int32_t>(HWC2::Error::None);
  }
}

/* Converts long __PRETTY_FUNCTION__ result, e.g.:
 * "int32_t android::LayerHook(hwc2_device_t *, hwc2_display_t, hwc2_layer_t,"
 * "Args...) [HookType = HWC2::Error (android::HwcLayer::*)(const native_handle"
 * "*,int), func = &android::HwcLayer::SetLayerBuffer, Args = <const
 * "native_handle, int>"
 * to the short "android::HwcLayer::SetLayerBuffer" for better logs readability
 */
static std::string GetFuncName(const char *pretty_function) {
  const std::string str(pretty_function);
  const char *start = "func = &";
  auto p1 = str.find(start);
  p1 += strlen(start);
  auto p2 = str.find(',', p1);
  return str.substr(p1, p2 - p1);
}

class Hwc2DeviceDisplay : public FrontendDisplayBase {
 public:
  std::vector<HwcDisplay::ReleaseFence> release_fences;
  std::vector<HwcDisplay::ChangedLayer> changed_layers;

  int64_t next_layer_id = 1;
};

static auto GetHwc2DeviceDisplay(HwcDisplay &display)
    -> std::shared_ptr<Hwc2DeviceDisplay> {
  auto frontend_private_data = display.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<Hwc2DeviceDisplay>();
    display.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<Hwc2DeviceDisplay>(frontend_private_data);
}

class Hwc2DeviceLayer : public FrontendLayerBase {
 public:
  auto HandleNextBuffer(buffer_handle_t buffer_handle, int32_t fence_fd)
      -> std::pair<std::optional<HwcLayer::LayerProperties>,
                   bool /* not a swapchain */> {
    auto slot = GetSlotNumber(buffer_handle);

    if (invalid_) {
      return std::make_pair(std::nullopt, true);
    }

    bool buffer_provided = false;
    bool not_a_swapchain = true;
    int32_t slot_id = 0;

    if (slot.has_value()) {
      buffer_provided = swchain_slots_[slot.value()];
      slot_id = slot.value();
      not_a_swapchain = true;
    }

    HwcLayer::LayerProperties lp;
    if (!buffer_provided) {
      auto bo_info = BufferInfoGetter::GetInstance()->GetBoInfo(buffer_handle);
      if (!bo_info) {
        invalid_ = true;
        return std::make_pair(std::nullopt, true);
      }

      lp.slot_buffer = {
          .slot_id = slot_id,
          .bi = bo_info,
      };
    }
    lp.active_slot = {
        .slot_id = slot_id,
        .fence = MakeSharedFd(fence_fd),
    };

    return std::make_pair(lp, not_a_swapchain);
  }

  void SwChainClearCache() {
    swchain_lookup_table_.clear();
    swchain_slots_.clear();
    swchain_reassembled_ = false;
  }

 private:
  auto GetSlotNumber(buffer_handle_t buffer_handle) -> std::optional<int32_t> {
    auto unique_id = BufferInfoGetter::GetInstance()->GetUniqueId(
        buffer_handle);
    if (!unique_id) {
      ALOGE("Failed to get unique id for buffer handle %p", buffer_handle);
      return std::nullopt;
    }

    if (swchain_lookup_table_.count(*unique_id) == 0) {
      SwChainReassemble(*unique_id);
      return std::nullopt;
    }

    if (!swchain_reassembled_) {
      return std::nullopt;
    }

    return swchain_lookup_table_[*unique_id];
  }

  void SwChainReassemble(BufferUniqueId unique_id) {
    if (swchain_lookup_table_.count(unique_id) != 0) {
      if (swchain_lookup_table_[unique_id] ==
          int(swchain_lookup_table_.size()) - 1) {
        /* Skip same buffer */
        return;
      }
      if (swchain_lookup_table_[unique_id] == 0) {
        swchain_reassembled_ = true;
        return;
      }
      /* Tracking error */
      SwChainClearCache();
      return;
    }

    swchain_lookup_table_[unique_id] = int(swchain_lookup_table_.size());
  }

  bool invalid_{}; /* Layer is invalid and should be skipped */
  std::map<BufferUniqueId, int /*slot*/> swchain_lookup_table_;
  std::map<int /*slot*/, bool /*buffer_provided*/> swchain_slots_;
  bool swchain_reassembled_{};
};

static auto GetHwc2DeviceLayer(HwcLayer &layer)
    -> std::shared_ptr<Hwc2DeviceLayer> {
  auto frontend_private_data = layer.GetFrontendPrivateData();
  if (!frontend_private_data) {
    frontend_private_data = std::make_shared<Hwc2DeviceLayer>();
    layer.SetFrontendPrivateData(frontend_private_data);
  }
  return std::static_pointer_cast<Hwc2DeviceLayer>(frontend_private_data);
}

struct Drmhwc2Device : hwc2_device {
  DrmHwcTwo drmhwctwo;
};

static DrmHwcTwo *ToDrmHwcTwo(hwc2_device_t *dev) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast):
  return &static_cast<Drmhwc2Device *>(dev)->drmhwctwo;
}

template <typename PFN, typename T>
static hwc2_function_pointer_t ToHook(T function) {
  // NOLINTNEXTLINE(modernize-type-traits): ToHook is going to be removed
  static_assert(std::is_same<PFN, T>::value, "Incompatible fn pointer");
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast):
  return reinterpret_cast<hwc2_function_pointer_t>(function);
}

template <typename T, typename HookType, HookType func, typename... Args>
static T DeviceHook(hwc2_device_t *dev, Args... args) {
  ALOGV("Device hook: %s", GetFuncName(__PRETTY_FUNCTION__).c_str());
  DrmHwcTwo *hwc = ToDrmHwcTwo(dev);
  const std::unique_lock lock(hwc->GetResMan().GetMainLock());
  return static_cast<T>(((*hwc).*func)(std::forward<Args>(args)...));
}

template <typename HookType, HookType func, typename... Args>
static int32_t DisplayHook(hwc2_device_t *dev, hwc2_display_t display_handle,
                           Args... args) {
  ALOGV("Display #%" PRIu64 " hook: %s", display_handle,
        GetFuncName(__PRETTY_FUNCTION__).c_str());
  DrmHwcTwo *hwc = ToDrmHwcTwo(dev);
  const std::unique_lock lock(hwc->GetResMan().GetMainLock());
  auto *display = hwc->GetDisplay(display_handle);
  if (display == nullptr)
    return static_cast<int32_t>(HWC2::Error::BadDisplay);

  return static_cast<int32_t>((display->*func)(std::forward<Args>(args)...));
}

static int HookDevClose(hw_device_t *dev) {
  // NOLINTNEXTLINE (cppcoreguidelines-pro-type-reinterpret-cast): Safe
  auto *hwc2_dev = reinterpret_cast<hwc2_device_t *>(dev);
  const std::unique_ptr<DrmHwcTwo> ctx(ToDrmHwcTwo(hwc2_dev));
  return 0;
}

static void HookDevGetCapabilities(hwc2_device_t * /*dev*/, uint32_t *out_count,
                                   int32_t * /*out_capabilities*/) {
  *out_count = 0;
}

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

#define LOCK_COMPOSER(dev)       \
  auto *ihwc = ToDrmHwcTwo(dev); \
  const std::unique_lock lock(ihwc->GetResMan().GetMainLock());

#define GET_DISPLAY(display_id)                  \
  auto *idisplay = ihwc->GetDisplay(display_id); \
  if (!idisplay)                                 \
    return static_cast<int32_t>(HWC2::Error::BadDisplay);

#define GET_LAYER(layer_id)                     \
  auto *ilayer = idisplay->get_layer(layer_id); \
  if (!ilayer)                                  \
    return static_cast<int32_t>(HWC2::Error::BadLayer);

// NOLINTEND(cppcoreguidelines-macro-usage)

static BufferColorSpace Hwc2ToColorSpace(int32_t dataspace) {
  switch (dataspace & HAL_DATASPACE_STANDARD_MASK) {
    case HAL_DATASPACE_STANDARD_BT709:
      return BufferColorSpace::kItuRec709;
    case HAL_DATASPACE_STANDARD_BT601_625:
    case HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED:
    case HAL_DATASPACE_STANDARD_BT601_525:
    case HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED:
      return BufferColorSpace::kItuRec601;
    case HAL_DATASPACE_STANDARD_BT2020:
    case HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE:
      return BufferColorSpace::kItuRec2020;
    default:
      return BufferColorSpace::kUndefined;
  }
}

static BufferSampleRange Hwc2ToSampleRange(int32_t dataspace) {
  switch (dataspace & HAL_DATASPACE_RANGE_MASK) {
    case HAL_DATASPACE_RANGE_FULL:
      return BufferSampleRange::kFullRange;
    case HAL_DATASPACE_RANGE_LIMITED:
      return BufferSampleRange::kLimitedRange;
    default:
      return BufferSampleRange::kUndefined;
  }
}

/* Device functions */
static int32_t Dump(hwc2_device_t *device, uint32_t *out_size,
                    char *out_buffer) {
  DrmHwcTwo *hwc = ToDrmHwcTwo(device);
  if (out_size == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (out_buffer != nullptr) {
    const std::string &last_dump = hwc->GetLastStateDump();
    auto copied_bytes = last_dump.copy(out_buffer, *out_size);
    *out_size = copied_bytes;
    return 0;
  }

  const std::string &new_dump = hwc->RefreshStateDump();
  *out_size = static_cast<uint32_t>(new_dump.size());
  return 0;
}

/* Display functions */
static int32_t CreateLayer(hwc2_device_t *device, hwc2_display_t display,
                           hwc2_layer_t *out_layer) {
  ALOGV("CreateLayer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  if (!idisplay->CreateLayer(hwc2display->next_layer_id)) {
    return static_cast<int32_t>(HWC2::Error::BadDisplay);
  }

  *out_layer = (hwc2_layer_t)hwc2display->next_layer_id;
  hwc2display->next_layer_id++;

  return 0;
}

static int32_t DestroyLayer(hwc2_device_t *device, hwc2_display_t display,
                            hwc2_layer_t layer) {
  ALOGV("DestroyLayer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (!idisplay->DestroyLayer((ILayerId)layer)) {
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  return 0;
}

static int32_t GetActiveConfig(hwc2_device_t *device, hwc2_display_t display,
                               hwc2_config_t *config) {
  ALOGV("GetActiveConfig");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  // If a config has been queued, it is considered the "active" config.
  const HwcDisplayConfig *hwc_config = idisplay->GetLastRequestedConfig();
  if (hwc_config == nullptr)
    return static_cast<int32_t>(HWC2::Error::BadConfig);

  *config = hwc_config->id;
  return 0;
}

static int32_t GetDisplayRequests(hwc2_device_t * /*device*/,
                                  hwc2_display_t /*display*/,
                                  int32_t * /* out_display_requests */,
                                  uint32_t *out_num_elements,
                                  hwc2_layer_t * /*out_layers*/,
                                  int32_t * /*out_layer_requests*/) {
  ALOGV("GetDisplayRequests");

  *out_num_elements = 0;
  return 0;
}

static int32_t GetDisplayType(hwc2_device_t *device, hwc2_display_t display,
                              int32_t *out_type) {
  ALOGV("GetDisplayType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  switch (idisplay->GetDisplayType()) {
    case HwcDisplay::DisplayType::kVirtual:
      *out_type = static_cast<int32_t>(HWC2::DisplayType::Virtual);
      break;
    case HwcDisplay::DisplayType::kInternal:
    case HwcDisplay::DisplayType::kExternal:
      *out_type = static_cast<int32_t>(HWC2::DisplayType::Physical);
      break;
  }
  return 0;
}

static int32_t GetDozeSupport(hwc2_device_t * /*device*/,
                              hwc2_display_t /*display*/,
                              int32_t *out_support) {
  ALOGV("GetDozeSupport");
  *out_support = 0;  // Doze support is not available
  return 0;
}

static int32_t GetClientTargetSupport(hwc2_device_t * /*device*/,
                                      hwc2_display_t /*display*/,
                                      uint32_t /*width*/, uint32_t /*height*/,
                                      int32_t /*format*/, int32_t dataspace) {
  ALOGV("GetClientTargetSupport");

  if (dataspace != HAL_DATASPACE_UNKNOWN)
    return static_cast<int32_t>(HWC2::Error::Unsupported);

  return 0;
}

static int32_t SetClientTarget(hwc2_device_t *device, hwc2_display_t display,
                               buffer_handle_t target, int32_t acquire_fence,
                               int32_t dataspace, hwc_region_t /*damage*/) {
  ALOGV("SetClientTarget");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto &client_layer = idisplay->GetClientLayer();
  auto h2l = GetHwc2DeviceLayer(client_layer);
  if (!h2l) {
    client_layer.SetFrontendPrivateData(std::make_shared<Hwc2DeviceLayer>());
  }

  if (target == nullptr) {
    client_layer.ClearSlots();
    h2l->SwChainClearCache();

    return 0;
  }

  auto [lp, not_a_swapchain] = h2l->HandleNextBuffer(target, acquire_fence);
  if (!lp) {
    ALOGE("Failed to process client target");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  if (not_a_swapchain) {
    client_layer.ClearSlots();
  }

  lp->color_space = Hwc2ToColorSpace(dataspace);
  lp->sample_range = Hwc2ToSampleRange(dataspace);

  idisplay->GetClientLayer().SetLayerProperties(lp.value());

  return 0;
}

static int32_t GetDisplayAttribute(hwc2_device_t *device,
                                   hwc2_display_t display, hwc2_config_t config,
                                   int32_t attribute, int32_t *value) {
  ALOGV("GetDisplayAttribute");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  const auto* hwc_config = idisplay->GetConfig(config);

  if (hwc_config == nullptr) {
    ALOGE("Could not find mode #%d", config);
    return static_cast<int32_t>(HWC2::Error::BadConfig);
  }

  int mm_width = -1;
  int mm_height = -1;
  std::tie(mm_width, mm_height) = idisplay->GetDisplayBoundsMm();
  std::optional<std::pair<float, float>> dpi_inches = {};

  if (mm_width > 0) {
    static const float kMmPerInch = 25.4;
    float dpi_x = float(hwc_config->mode.GetRawMode().hdisplay) * kMmPerInch /
                  float(mm_width);
    float dpi_y = mm_height <= 0
                      ? dpi_x
                      : float(hwc_config->mode.GetRawMode().vdisplay) *
                            kMmPerInch / float(mm_height);
    dpi_inches = std::make_pair(dpi_x, dpi_y);
  }

  static const int kLegacyDpiUnit = 1000;
  switch (static_cast<HWC2::Attribute>(attribute)) {
    case HWC2::Attribute::Width:
      *value = static_cast<int>(hwc_config->mode.GetRawMode().hdisplay);
      break;
    case HWC2::Attribute::Height:
      *value = static_cast<int>(hwc_config->mode.GetRawMode().vdisplay);
      break;
    case HWC2::Attribute::VsyncPeriod:
      // in nanoseconds
      *value = hwc_config->mode.GetVSyncPeriodNs();
      break;
    case HWC2::Attribute::DpiY:
      *value = dpi_inches
                   ? static_cast<int>(dpi_inches->second * kLegacyDpiUnit)
                   : -1;
      break;
    case HWC2::Attribute::DpiX:
      *value = dpi_inches ? static_cast<int>(dpi_inches->first * kLegacyDpiUnit)
                          : -1;
      break;
#if __ANDROID_API__ > 29
    case HWC2::Attribute::ConfigGroup:
      /* Dispite ConfigGroup is a part of HWC2.4 API, framework
       * able to request it even if service @2.1 is used */
      *value = int(hwc_config->group_id);
      break;
#endif
    default:
      *value = -1;
      return static_cast<int32_t>(HWC2::Error::BadConfig);
  }
  return 0;
}

static int32_t GetDisplayConfigs(hwc2_device_t *device, hwc2_display_t display,
                                 uint32_t *num_configs,
                                 hwc2_config_t *configs) {
  ALOGV("GetDisplayConfigs");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  uint32_t idx = 0;
  for (const auto &hwc_config : idisplay->GetDisplayConfigs().hwc_configs) {
    if (hwc_config.second.disabled) {
      continue;
    }

    if (configs != nullptr) {
      if (idx >= *num_configs) {
        break;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
      configs[idx] = hwc_config.second.id;
    }

    idx++;
  }
  *num_configs = idx;
  return 0;
}

static int32_t GetDisplayName(hwc2_device_t *device, hwc2_display_t display,
                              uint32_t *size, char *name) {
  ALOGV("GetDisplayName");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  std::string name_str = idisplay->GetDisplayName();

  auto length = name_str.length();
  if (name == nullptr) {
    *size = length;
    return 0;
  }

  *size = std::min<uint32_t>(static_cast<uint32_t>(length - 1), *size);
  strncpy(name, name_str.c_str(), *size);
  return 0;
}
static int32_t SetOutputBuffer(hwc2_device_t *device, hwc2_display_t display,
                               buffer_handle_t buffer, int32_t release_fence) {
  ALOGV("SetOutputBuffer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto &writeback_layer = idisplay->GetWritebackLayer();
  if (!writeback_layer) {
    ALOGE("Writeback layer is not available");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  auto h2l = GetHwc2DeviceLayer(*writeback_layer);
  if (!h2l) {
    writeback_layer->SetFrontendPrivateData(
        std::make_shared<Hwc2DeviceLayer>());
  }

  auto [lp, not_a_swapchain] = h2l->HandleNextBuffer(buffer, release_fence);
  if (!lp) {
    ALOGE("Failed to process output buffer");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  if (not_a_swapchain) {
    writeback_layer->ClearSlots();
  }

  writeback_layer->SetLayerProperties(lp.value());

  return 0;
}

static int32_t AcceptDisplayChanges(hwc2_device_t *device,
                                    hwc2_display_t display) {
  ALOGV("AcceptDisplayChanges");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  idisplay->AcceptValidatedComposition();

  return 0;
}

static int32_t GetReleaseFences(hwc2_device_t *device, hwc2_display_t display,
                                uint32_t *out_num_elements,
                                hwc2_layer_t *out_layers, int32_t *out_fences) {
  ALOGV("GetReleaseFences");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  if (*out_num_elements < hwc2display->release_fences.size()) {
    ALOGW("Overflow num_elements %d/%zu", *out_num_elements,
          hwc2display->release_fences.size());
    return static_cast<int32_t>(HWC2::Error::NoResources);
  }

  for (size_t i = 0; i < hwc2display->release_fences.size(); ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_layers[i] = hwc2display->release_fences[i].first;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_fences[i] = DupFd(hwc2display->release_fences[i].second);
  }

  *out_num_elements = hwc2display->release_fences.size();
  hwc2display->release_fences.clear();

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetPowerMode(hwc2_device_t *device, hwc2_display_t display,
                            int32_t mode) {
  ALOGV("SetPowerMode");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  switch (mode) {
    // Supported modes.
    case static_cast<int32_t>(HWC2::PowerMode::Off):
    case static_cast<int32_t>(HWC2::PowerMode::On):
      break;
    // Unsupported modes.
    case static_cast<int32_t>(HWC2::PowerMode::Doze):
    case static_cast<int32_t>(HWC2::PowerMode::DozeSuspend):
      return static_cast<int32_t>(HWC2::Error::Unsupported);
    // Bad parameter.
    default:
      ALOGE("Incorrect power mode value (%d)\n", mode);
      return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (!idisplay->SetDisplayEnabled(mode ==
                                   static_cast<int32_t>(HWC2::PowerMode::On))) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetVsyncEnabled(hwc2_device_t *device, hwc2_display_t display,
                               int32_t enabled) {
  ALOGV("SetVsyncEnabled");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  idisplay->SetVsyncCallbacksEnabled(HWC2_VSYNC_ENABLE == enabled);
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t ValidateDisplay(hwc2_device_t *device, hwc2_display_t display,
                               uint32_t *out_num_types,
                               uint32_t *out_num_requests) {
  ALOGV("ValidateDisplay");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  hwc2display->changed_layers = idisplay->ValidateStagedComposition();

  *out_num_types = hwc2display->changed_layers.size();
  *out_num_requests = 0;

  return 0;
}

static int32_t GetChangedCompositionTypes(hwc2_device_t *device,
                                          hwc2_display_t display,
                                          uint32_t *out_num_elements,
                                          hwc2_layer_t *out_layers,
                                          int32_t *out_types) {
  ALOGV("GetChangedCompositionTypes");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  if (*out_num_elements < hwc2display->changed_layers.size()) {
    ALOGW("Overflow num_elements %d/%zu", *out_num_elements,
          hwc2display->changed_layers.size());
    return static_cast<int32_t>(HWC2::Error::NoResources);
  }

  for (size_t i = 0; i < hwc2display->changed_layers.size(); ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_layers[i] = hwc2display->changed_layers[i].first;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
    out_types[i] = static_cast<int32_t>(hwc2display->changed_layers[i].second);
  }

  *out_num_elements = hwc2display->changed_layers.size();
  hwc2display->changed_layers.clear();

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t PresentDisplay(hwc2_device_t *device, hwc2_display_t display,
                              int32_t *out_release_fence) {
  ALOGV("PresentDisplay");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto hwc2display = GetHwc2DeviceDisplay(*idisplay);

  SharedFd out_fence;

  hwc2display->release_fences.clear();

  if (!idisplay->PresentStagedComposition(std::nullopt, out_fence,
                                          hwc2display->release_fences)) {
    ALOGE("Failed to present display");
    return static_cast<int32_t>(HWC2::Error::BadDisplay);
  }

  *out_release_fence = DupFd(out_fence);

  return 0;
}

static int32_t SetActiveConfig(hwc2_device_t *device, hwc2_display_t display,
                               hwc2_config_t config) {
  ALOGV("SetActiveConfig");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  QueuedConfigTiming out_timing{};
  auto result = idisplay->QueueConfig(config,
                                      ResourceManager::GetTimeMonotonicNs(),
                                      false, &out_timing);
  return ConfigErrorToHWC2(result);
}

#if __ANDROID_API__ >= 28

static int32_t GetDisplayBrightnessSupport(hwc2_device_t * /*device*/,
                                           hwc2_display_t /*display*/,
                                           bool *out_support) {
  ALOGV("GetDisplayBrightnessSupport");
  *out_support = false;  // Brightness support is not available
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetDisplayBrightness(hwc2_device_t * /*device*/,
                                    hwc2_display_t /*display*/,
                                    float /*brightness*/) {
  ALOGV("SetDisplayBrightness");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetDisplayIdentificationData(hwc2_device_t *device,
                                            hwc2_display_t display,
                                            uint8_t *out_port,
                                            uint32_t *out_data_size,
                                            uint8_t *out_data) {
  ALOGV("GetDisplayIdentificationData");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  auto edid = idisplay->GetRawEdid();
  if (edid.empty()) {
    return static_cast<int32_t>(HWC2::Error::Unsupported);
  }

  *out_port = idisplay->GetPort();

  if (out_data != nullptr) {
    *out_data_size = std::min(*out_data_size,
                              static_cast<uint32_t>(edid.size()));
    memcpy(out_data, edid.data(), *out_data_size);
  } else {
    *out_data_size = edid.size();
  }

  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t GetDisplayCapabilities(hwc2_device_t *device,
                                      hwc2_display_t display,
                                      uint32_t *out_num_capabilities,
                                      uint32_t *out_capabilities) {
  ALOGV("GetDisplayCapabilities");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (out_num_capabilities == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (ihwc->GetResMan().GetCtmHandling() == CtmHandling::kDrmOrIgnore) {
    if (out_capabilities != nullptr && *out_num_capabilities > 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic):
      out_capabilities[0] = HWC2_DISPLAY_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
    }
    *out_num_capabilities = 1;
  }

  return static_cast<int32_t>(HWC2::Error::None);
}

#endif

#if __ANDROID_API__ >= 29
static int32_t GetDisplayConnectionType(hwc2_device_t *device,
                                        hwc2_display_t display,
                                        int32_t *out_connection_type) {
  ALOGV("GetDisplayConnectionType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  switch (idisplay->GetDisplayType()) {
    case HwcDisplay::DisplayType::kVirtual:
      return static_cast<int32_t>(HWC2::Error::BadDisplay);
    case HwcDisplay::DisplayType::kInternal:
      *out_connection_type = static_cast<int32_t>(
          HWC2::DisplayConnectionType::Internal);
      break;
    case HwcDisplay::DisplayType::kExternal:
      *out_connection_type = static_cast<int32_t>(
          HWC2::DisplayConnectionType::External);
      break;
  }
  return 0;
}

static int32_t GetDisplayVsyncPeriod(hwc2_device_t *device,
                                     hwc2_display_t display,
                                     hwc2_vsync_period_t *out_vsync_period) {
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  const HwcDisplayConfig *config = idisplay->GetCurrentConfig();
  if (config == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadConfig);
  }

  *out_vsync_period = config->mode.GetVSyncPeriodNs();
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetActiveConfigWithConstraints(
    hwc2_device_t *device, hwc2_display_t display, hwc2_config_t config,
    hwc_vsync_period_change_constraints_t *vsync_period_change_constraints,
    hwc_vsync_period_change_timeline_t *out_timeline) {
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (vsync_period_change_constraints == nullptr || out_timeline == nullptr) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  if (vsync_period_change_constraints->seamlessRequired != 0) {
    return static_cast<int32_t>(HWC2::Error::SeamlessNotAllowed);
  }

  QueuedConfigTiming out_timing{};
  auto result = idisplay->QueueConfig(config,
                                      vsync_period_change_constraints
                                          ->desiredTimeNanos,
                                      false, &out_timing);

  out_timeline->newVsyncAppliedTimeNanos = out_timing.new_vsync_time_ns;
  out_timeline->refreshTimeNanos = out_timing.refresh_time_ns;
  out_timeline->refreshRequired = 1;

  return ConfigErrorToHWC2(result);
}

static int32_t SetAutoLowLatencyMode(hwc2_device_t * /*device*/,
                                     hwc2_display_t /*display*/, bool /*on*/) {
  ALOGV("SetAutoLowLatencyMode");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t GetSupportedContentTypes(
    hwc2_device_t * /*device*/, hwc2_display_t /*display*/,
    uint32_t *out_num_supported_content_types,
    uint32_t * /*out_supported_content_types*/) {
  ALOGV("GetSupportedContentTypes");
  *out_num_supported_content_types = 0;
  return static_cast<int32_t>(HWC2::Error::None);
}

static int32_t SetContentType(hwc2_device_t *device, hwc2_display_t display,
                              int32_t content_type) {
  ALOGV("SetContentType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);

  if (content_type < HWC2_CONTENT_TYPE_NONE ||
      content_type > HWC2_CONTENT_TYPE_GAME) {
    return static_cast<int32_t>(HWC2::Error::BadParameter);
  }

  idisplay->SetContentType(static_cast<ContentType>(content_type));

  return static_cast<int32_t>(HWC2::Error::None);
}
#endif

/* Layer functions */

static int32_t SetLayerBlendMode(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer,
                                 int32_t /*hwc2_blend_mode_t*/ mode) {
  ALOGV("SetLayerBlendMode");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  BufferBlendMode blend_mode{};
  switch (static_cast<HWC2::BlendMode>(mode)) {
    case HWC2::BlendMode::None:
      blend_mode = BufferBlendMode::kNone;
      break;
    case HWC2::BlendMode::Premultiplied:
      blend_mode = BufferBlendMode::kPreMult;
      break;
    case HWC2::BlendMode::Coverage:
      blend_mode = BufferBlendMode::kCoverage;
      break;
    default:
      ALOGE("Unknown blending mode b=%d", mode);
      blend_mode = BufferBlendMode::kUndefined;
      break;
  }

  HwcLayer::LayerProperties layer_properties;
  layer_properties.blend_mode = blend_mode;

  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerBuffer(hwc2_device_t *device, hwc2_display_t display,
                              hwc2_layer_t layer, buffer_handle_t buffer,
                              int32_t acquire_fence) {
  ALOGV("SetLayerBuffer");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  auto h2l = GetHwc2DeviceLayer(*ilayer);

  auto [lp, not_a_swapchain] = h2l->HandleNextBuffer(buffer, acquire_fence);
  if (!lp) {
    ALOGV("Failed to process layer buffer");
    return static_cast<int32_t>(HWC2::Error::BadLayer);
  }

  if (not_a_swapchain) {
    ilayer->ClearSlots();
  }

  ilayer->SetLayerProperties(lp.value());

  return 0;
}

static int32_t SetLayerDataspace(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer,
                                 int32_t /*android_dataspace_t*/ dataspace) {
  ALOGV("SetLayerDataspace");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.color_space = Hwc2ToColorSpace(dataspace);
  layer_properties.sample_range = Hwc2ToSampleRange(dataspace);
  ilayer->SetLayerProperties(layer_properties);
  return 0;
}

static int32_t SetCursorPosition(hwc2_device_t * /*device*/,
                                 hwc2_display_t /*display*/,
                                 hwc2_layer_t /*layer*/, int32_t /*x*/,
                                 int32_t /*y*/) {
  ALOGV("SetCursorPosition");
  return 0;
}

static int32_t SetLayerColor(hwc2_device_t * /*device*/,
                             hwc2_display_t /*display*/, hwc2_layer_t /*layer*/,
                             hwc_color_t /*color*/) {
  ALOGV("SetLayerColor");
  return 0;
}

static int32_t SetLayerCompositionType(hwc2_device_t *device,
                                       hwc2_display_t display,
                                       hwc2_layer_t layer,
                                       int32_t /*hwc2_composition_t*/ type) {
  ALOGV("SetLayerCompositionType");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  switch (static_cast<HWC2::Composition>(type)) {
    case HWC2::Composition::Client:
      layer_properties.composition_type = HwcLayer::CompositionType::kClient;
      break;
    case HWC2::Composition::Device:
      layer_properties.composition_type = HwcLayer::CompositionType::kDevice;
      break;
    case HWC2::Composition::SolidColor:
      layer_properties
          .composition_type = HwcLayer::CompositionType::kSolidColor;
      break;
    case HWC2::Composition::Cursor:
      layer_properties.composition_type = HwcLayer::CompositionType::kCursor;
      break;
    default:
      ALOGE("Unsupported composition type t=%d", type);
      break;
  }
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerDisplayFrame(hwc2_device_t *device,
                                    hwc2_display_t display, hwc2_layer_t layer,
                                    hwc_rect_t frame) {
  ALOGV("SetLayerDisplayFrame");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.display_frame = {.i_rect = IRect{.left = frame.left,
                                                    .top = frame.top,
                                                    .right = frame.right,
                                                    .bottom = frame.bottom}};
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerPlaneAlpha(hwc2_device_t *device, hwc2_display_t display,
                                  hwc2_layer_t layer, float alpha) {
  ALOGV("SetLayerPlaneAlpha");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.alpha = alpha;
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerSidebandStream(hwc2_device_t * /*device*/,
                                      hwc2_display_t /*display*/,
                                      hwc2_layer_t /*layer*/,
                                      const native_handle_t * /*stream*/) {
  ALOGV("SetLayerSidebandStream");
  return static_cast<int32_t>(HWC2::Error::Unsupported);
}

static int32_t SetLayerSourceCrop(hwc2_device_t *device, hwc2_display_t display,
                                  hwc2_layer_t layer, hwc_frect_t crop) {
  ALOGV("SetLayerSourceCrop");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.source_crop = {
      .f_rect = SrcRectInfo::FRect{.left = crop.left,
                                   .top = crop.top,
                                   .right = crop.right,
                                   .bottom = crop.bottom}};
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerSurfaceDamage(hwc2_device_t *device,
                                     hwc2_display_t display, hwc2_layer_t layer,
                                     hwc_region_t damage) {
  ALOGV("SetLayerSurfaceDamage");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties{.damage = DamageInfo{}};
  for (size_t i = 0; i < damage.numRects; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto rect = damage.rects[i];
    layer_properties.damage->dmg_rects.emplace_back(
        IRect{.left = rect.left,
              .top = rect.top,
              .right = rect.right,
              .bottom = rect.bottom});
  }
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerTransform(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer, int32_t transform) {
  ALOGV("SetLayerTransform");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.transform = {
      .hflip = (transform & HAL_TRANSFORM_FLIP_H) != 0,
      .vflip = (transform & HAL_TRANSFORM_FLIP_V) != 0,
      .rotate90 = (transform & HAL_TRANSFORM_ROT_90) != 0,
  };
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

static int32_t SetLayerVisibleRegion(hwc2_device_t * /*device*/,
                                     hwc2_display_t /*display*/,
                                     hwc2_layer_t /*layer*/,
                                     hwc_region_t /*visible*/) {
  ALOGV("SetLayerVisibleRegion");
  return 0;
}

static int32_t SetLayerZOrder(hwc2_device_t *device, hwc2_display_t display,
                              hwc2_layer_t layer, uint32_t z) {
  ALOGV("SetLayerZOrder");
  LOCK_COMPOSER(device);
  GET_DISPLAY(display);
  GET_LAYER(layer);

  HwcLayer::LayerProperties layer_properties;
  layer_properties.z_order = z;
  ilayer->SetLayerProperties(layer_properties);

  return 0;
}

/* Entry point for the HWC2 API */
// NOLINTBEGIN(cppcoreguidelines-pro-type-cstyle-cast)

static hwc2_function_pointer_t HookDevGetFunction(struct hwc2_device * /*dev*/,
                                                  int32_t descriptor) {
  auto func = static_cast<HWC2::FunctionDescriptor>(descriptor);
  switch (func) {
    // Device functions
    case HWC2::FunctionDescriptor::CreateVirtualDisplay:
      return ToHook<HWC2_PFN_CREATE_VIRTUAL_DISPLAY>(
          DeviceHook<int32_t, decltype(&DrmHwcTwo::CreateVirtualDisplay),
                     &DrmHwcTwo::CreateVirtualDisplay, uint32_t, uint32_t,
                     int32_t *, hwc2_display_t *>);
    case HWC2::FunctionDescriptor::DestroyVirtualDisplay:
      return ToHook<HWC2_PFN_DESTROY_VIRTUAL_DISPLAY>(
          DeviceHook<int32_t, decltype(&DrmHwcTwo::DestroyVirtualDisplay),
                     &DrmHwcTwo::DestroyVirtualDisplay, hwc2_display_t>);
    case HWC2::FunctionDescriptor::Dump:
      return (hwc2_function_pointer_t)Dump;
    case HWC2::FunctionDescriptor::GetMaxVirtualDisplayCount:
      return ToHook<HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT>(
          DeviceHook<uint32_t, decltype(&DrmHwcTwo::GetMaxVirtualDisplayCount),
                     &DrmHwcTwo::GetMaxVirtualDisplayCount>);
    case HWC2::FunctionDescriptor::RegisterCallback:
      return ToHook<HWC2_PFN_REGISTER_CALLBACK>(
          DeviceHook<int32_t, decltype(&DrmHwcTwo::RegisterCallback),
                     &DrmHwcTwo::RegisterCallback, int32_t,
                     hwc2_callback_data_t, hwc2_function_pointer_t>);

    // Display functions
    case HWC2::FunctionDescriptor::AcceptDisplayChanges:
      return (hwc2_function_pointer_t)AcceptDisplayChanges;
    case HWC2::FunctionDescriptor::CreateLayer:
      return (hwc2_function_pointer_t)CreateLayer;
    case HWC2::FunctionDescriptor::DestroyLayer:
      return (hwc2_function_pointer_t)DestroyLayer;
    case HWC2::FunctionDescriptor::GetActiveConfig:
      return (hwc2_function_pointer_t)GetActiveConfig;
    case HWC2::FunctionDescriptor::GetChangedCompositionTypes:
      return (hwc2_function_pointer_t)GetChangedCompositionTypes;
    case HWC2::FunctionDescriptor::GetClientTargetSupport:
      return (hwc2_function_pointer_t)GetClientTargetSupport;
    case HWC2::FunctionDescriptor::GetColorModes:
      return ToHook<HWC2_PFN_GET_COLOR_MODES>(
          DisplayHook<decltype(&HwcDisplay::GetColorModes),
                      &HwcDisplay::GetColorModes, uint32_t *, int32_t *>);
    case HWC2::FunctionDescriptor::GetDisplayAttribute:
      return (hwc2_function_pointer_t)GetDisplayAttribute;
    case HWC2::FunctionDescriptor::GetDisplayConfigs:
      return (hwc2_function_pointer_t)GetDisplayConfigs;
    case HWC2::FunctionDescriptor::GetDisplayName:
      return (hwc2_function_pointer_t)GetDisplayName;
    case HWC2::FunctionDescriptor::GetDisplayRequests:
      return (hwc2_function_pointer_t)GetDisplayRequests;
    case HWC2::FunctionDescriptor::GetDisplayType:
      return (hwc2_function_pointer_t)GetDisplayType;
    case HWC2::FunctionDescriptor::GetDozeSupport:
      return (hwc2_function_pointer_t)GetDozeSupport;
    case HWC2::FunctionDescriptor::GetHdrCapabilities:
      return ToHook<HWC2_PFN_GET_HDR_CAPABILITIES>(
          DisplayHook<decltype(&HwcDisplay::GetHdrCapabilities),
                      &HwcDisplay::GetHdrCapabilities, uint32_t *, int32_t *,
                      float *, float *, float *>);
    case HWC2::FunctionDescriptor::GetReleaseFences:
      return (hwc2_function_pointer_t)GetReleaseFences;
    case HWC2::FunctionDescriptor::PresentDisplay:
      return (hwc2_function_pointer_t)PresentDisplay;
    case HWC2::FunctionDescriptor::SetActiveConfig:
      return (hwc2_function_pointer_t)SetActiveConfig;
    case HWC2::FunctionDescriptor::SetClientTarget:
      return (hwc2_function_pointer_t)SetClientTarget;
    case HWC2::FunctionDescriptor::SetColorMode:
      return ToHook<HWC2_PFN_SET_COLOR_MODE>(
          DisplayHook<decltype(&HwcDisplay::SetColorMode),
                      &HwcDisplay::SetColorMode, int32_t>);
    case HWC2::FunctionDescriptor::SetColorTransform:
      return ToHook<HWC2_PFN_SET_COLOR_TRANSFORM>(
          DisplayHook<decltype(&HwcDisplay::SetColorTransform),
                      &HwcDisplay::SetColorTransform, const float *, int32_t>);
    case HWC2::FunctionDescriptor::SetOutputBuffer:
      return (hwc2_function_pointer_t)SetOutputBuffer;
    case HWC2::FunctionDescriptor::SetPowerMode:
      return (hwc2_function_pointer_t)SetPowerMode;
    case HWC2::FunctionDescriptor::SetVsyncEnabled:
      return (hwc2_function_pointer_t)SetVsyncEnabled;
    case HWC2::FunctionDescriptor::ValidateDisplay:
      return (hwc2_function_pointer_t)ValidateDisplay;
#if __ANDROID_API__ > 27
    case HWC2::FunctionDescriptor::GetRenderIntents:
      return ToHook<HWC2_PFN_GET_RENDER_INTENTS>(
          DisplayHook<decltype(&HwcDisplay::GetRenderIntents),
                      &HwcDisplay::GetRenderIntents, int32_t, uint32_t *,
                      int32_t *>);
    case HWC2::FunctionDescriptor::SetColorModeWithRenderIntent:
      return ToHook<HWC2_PFN_SET_COLOR_MODE_WITH_RENDER_INTENT>(
          DisplayHook<decltype(&HwcDisplay::SetColorModeWithIntent),
                      &HwcDisplay::SetColorModeWithIntent, int32_t, int32_t>);
#endif
#if __ANDROID_API__ > 28
    case HWC2::FunctionDescriptor::GetDisplayIdentificationData:
      return (hwc2_function_pointer_t)GetDisplayIdentificationData;
    case HWC2::FunctionDescriptor::GetDisplayCapabilities:
      return (hwc2_function_pointer_t)GetDisplayCapabilities;
    case HWC2::FunctionDescriptor::GetDisplayBrightnessSupport:
      return (hwc2_function_pointer_t)GetDisplayBrightnessSupport;
    case HWC2::FunctionDescriptor::SetDisplayBrightness:
      return (hwc2_function_pointer_t)SetDisplayBrightness;
#endif /* __ANDROID_API__ > 28 */
#if __ANDROID_API__ > 29
    case HWC2::FunctionDescriptor::GetDisplayConnectionType:
      return (hwc2_function_pointer_t)GetDisplayConnectionType;
    case HWC2::FunctionDescriptor::GetDisplayVsyncPeriod:
      return (hwc2_function_pointer_t)GetDisplayVsyncPeriod;
    case HWC2::FunctionDescriptor::SetActiveConfigWithConstraints:
      return (hwc2_function_pointer_t)SetActiveConfigWithConstraints;
    case HWC2::FunctionDescriptor::SetAutoLowLatencyMode:
      return (hwc2_function_pointer_t)SetAutoLowLatencyMode;
    case HWC2::FunctionDescriptor::GetSupportedContentTypes:
      return (hwc2_function_pointer_t)GetSupportedContentTypes;
    case HWC2::FunctionDescriptor::SetContentType:
      return (hwc2_function_pointer_t)SetContentType;
#endif
    // Layer functions
    case HWC2::FunctionDescriptor::SetCursorPosition:
      return (hwc2_function_pointer_t)SetCursorPosition;
    case HWC2::FunctionDescriptor::SetLayerBlendMode:
      return (hwc2_function_pointer_t)SetLayerBlendMode;
    case HWC2::FunctionDescriptor::SetLayerBuffer:
      return (hwc2_function_pointer_t)SetLayerBuffer;
    case HWC2::FunctionDescriptor::SetLayerColor:
      return (hwc2_function_pointer_t)SetLayerColor;
    case HWC2::FunctionDescriptor::SetLayerCompositionType:
      return (hwc2_function_pointer_t)SetLayerCompositionType;
    case HWC2::FunctionDescriptor::SetLayerDataspace:
      return (hwc2_function_pointer_t)SetLayerDataspace;
    case HWC2::FunctionDescriptor::SetLayerDisplayFrame:
      return (hwc2_function_pointer_t)SetLayerDisplayFrame;
    case HWC2::FunctionDescriptor::SetLayerPlaneAlpha:
      return (hwc2_function_pointer_t)SetLayerPlaneAlpha;
    case HWC2::FunctionDescriptor::SetLayerSidebandStream:
      return (hwc2_function_pointer_t)SetLayerSidebandStream;
    case HWC2::FunctionDescriptor::SetLayerSourceCrop:
      return (hwc2_function_pointer_t)SetLayerSourceCrop;
    case HWC2::FunctionDescriptor::SetLayerSurfaceDamage:
      return (hwc2_function_pointer_t)SetLayerSurfaceDamage;
    case HWC2::FunctionDescriptor::SetLayerTransform:
      return (hwc2_function_pointer_t)SetLayerTransform;
    case HWC2::FunctionDescriptor::SetLayerVisibleRegion:
      return (hwc2_function_pointer_t)SetLayerVisibleRegion;
    case HWC2::FunctionDescriptor::SetLayerZOrder:
      return (hwc2_function_pointer_t)SetLayerZOrder;
    case HWC2::FunctionDescriptor::Invalid:
    default:
      return nullptr;
  }
}

// NOLINTEND(cppcoreguidelines-pro-type-cstyle-cast)

static int HookDevOpen(const struct hw_module_t *module, const char *name,
                       struct hw_device_t **dev) {
  if (strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
    ALOGE("Invalid module name- %s", name);
    return -EINVAL;
  }

  auto ctx = std::make_unique<Drmhwc2Device>();
  if (!ctx) {
    ALOGE("Failed to allocate DrmHwcTwo");
    return -ENOMEM;
  }

  ctx->common.tag = HARDWARE_DEVICE_TAG;
  ctx->common.version = HWC_DEVICE_API_VERSION_2_0;
  ctx->common.close = HookDevClose;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  ctx->common.module = (hw_module_t *)module;
  ctx->getCapabilities = HookDevGetCapabilities;
  ctx->getFunction = HookDevGetFunction;

  *dev = &ctx.release()->common;

  return 0;
}

}  // namespace android

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static struct hw_module_methods_t hwc2_module_methods = {
    .open = android::HookDevOpen,
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MODULE_API_VERSION(2, 0),
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "DrmHwcTwo module",
    .author = "The Android Open Source Project",
    .methods = &hwc2_module_methods,
    .dso = nullptr,
    .reserved = {0},
};
