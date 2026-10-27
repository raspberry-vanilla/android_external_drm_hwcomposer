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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "HwcDisplay.h"

#include <cutils/trace.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <ui/ColorSpace.h>
#include <utils/Trace.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "backend/Backend.h"
#include "backend/BackendDisplayCapabilities.h"
#include "backend/BackendManager.h"
#include "bufferinfo/BufferInfo.h"
#include "compositor/CompositionPlanner.h"
#include "compositor/DisplayInfo.h"
#include "compositor/FlatteningController.h"
#include "compositor/HdcpController.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "compositor/PresentedCompositionCache.h"
#include "drm/CommitStatus.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmHwc.h"
#include "drm/DrmPlane.h"
#include "drm/VSyncWorker.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "stats/DisplayConfigurationResultReporter.h"
#include "stats/DisplayHotplugConnectModeDetectedAtomReporter.h"
#include "stats/Stats.h"
#include "utils/ColorUtil.h"
#include "utils/EdidWrapper.h"
#include "utils/SysfsBacklightController.h"
#include "utils/fd.h"
#include "utils/log.h"
#include "utils/math.h"
#include "utils/properties.h"

#ifdef ANDROID
#include <ui/GraphicTypes.h>
#else
// <ui/GraphicTypes.h> and <android/hardware/graphics/common/1.1/types.h> pull
// in <hidl/HidlInternal.h>, which fails a static_assert on sizeof(std::string)
// when compiled against host GNU libstdc++ in drm_hwcomposer_unittests (see
// commit 7aeb2ddcf7). Provide the enum definition corresponding to the forward
// declaration in HwcDisplay.h for !ANDROID host builds.
namespace android::hardware::graphics::common::V1_1 {
enum class RenderIntent : int32_t {
  COLORIMETRIC = 0,
  ENHANCE = 1,
  TONE_MAP_COLORIMETRIC = 2,
  TONE_MAP_ENHANCE = 3,
};
}  // namespace android::hardware::graphics::common::V1_1
#endif

namespace android::drm_hwcomposer {

using FlattenReason = CompositionPlanner::FlattenReason;

namespace {

// NOLINTBEGIN(misc-include-cleaner)
constexpr auto kFlatteningTimeout = 1s;
// 6s accommodates the DP HDCP 2.2 repeater spec limit (t_rxlist <= 3.0s),
// AKE protocol overhead, and driver retries matching Intel IGT test allowances.
constexpr auto kHdcpRetryTimeout = 6s;
// NOLINTEND(misc-include-cleaner)

// Client target buffer may be updated since the composition was validated,
// so get the latest LayerData.
void UpdateClientTargetIfNeeded(const HwcLayer &client_layer,
                                LayerToPlaneJoiningPlan *composition_plan) {
  if (!composition_plan)
    return;
  if (const auto client_z = composition_plan->client_z_order)
    composition_plan->plan[*client_z].layer = client_layer.GetLayerData();
}

bool IsUnscaledFullScreenDeviceLayer(const HwcLayer &layer, uint32_t width,
                                     uint32_t height) {
  if (!layer.IsLayerUsableAsDevice()) {
    return false;
  }
  const LayerData &layer_data = layer.GetLayerData();
  if (!layer_data.bi || layer_data.bi->width != width ||
      layer_data.bi->height != height ||
      layer_data.pi.RequireScalingOrPhasing()) {
    return false;
  }
  const IRect full_screen_rect{0, 0, static_cast<int32_t>(width),
                               static_cast<int32_t>(height)};
  return !layer_data.pi.display_frame.i_rect ||
         *layer_data.pi.display_frame.i_rect == full_screen_rect;
}

}  // namespace

auto HwcDisplay::GetDisplayName() const -> std::string {
  std::ostringstream stream;
  if (IsInHeadlessMode()) {
    stream << "null-display";
  } else {
    stream << "display-" << GetPipe().connector->Get()->GetId();
  }
  return stream.str();
}

auto HwcDisplay::GetDisplayConfigs() const -> std::vector<HwcDisplayConfig> {
  std::vector<HwcDisplayConfig> display_configs;
  display_configs.reserve(configs_.hwc_configs.size());
  for (const auto &[_, config] : configs_.hwc_configs) {
    display_configs.emplace_back(config);
  }

  return display_configs;
}

HwcDisplay::HwcDisplay(DisplayHandle handle, bool is_virtual, DrmHwc *hwc)
    : hwc_(hwc), handle_(handle), is_virtual_(is_virtual), client_layer_(this) {
  // Create writeback layer for both virtual displays and potential readback
  // operations
  writeback_layer_ = std::make_unique<HwcLayer>(this);

  display_mode_reporter_ = DisplayHotplugConnectModeDetectedAtomReporter::
      Create();
  config_result_reporter_ = DisplayConfigurationResultReporter::Create();
}

void HwcDisplay::SetColorTransformMatrix(
    const HalColorTransformMatrix &color_transform_matrix) {
  if (IsInHeadlessMode())
    return;

  const bool
      color_transform_is_identity = std::equal(color_transform_matrix.begin(),
                                               color_transform_matrix.end(),
                                               kIdentityMatrix.begin(),
                                               FloatEquals);
  if (color_transform_is_identity) {
    client_color_matrix_ = GetIdentityCtmPtr();
    client_ctm_has_offset_ = false;
  } else {
    client_ctm_has_offset_ = ColorUtil::HasOffset(color_transform_matrix);
    client_color_matrix_ = std::make_shared<HalColorTransformMatrix>(
        use_color_pipeline_
            ? ColorUtil::ToLinearCtm(color_transform_matrix, color_mode_)
            : color_transform_matrix);
  }

  UpdateColorTransformMatrix();
}

void HwcDisplay::UpdateColorTransformMatrix() {
  color_matrix_ = ColorUtil::Multiply(render_intent_matrix_,
                                      client_color_matrix_);
}

HwcDisplay::~HwcDisplay() {
  Deinit();
};

auto HwcDisplay::GetConfig(ConfigId config_id) const
    -> const HwcDisplayConfig * {
  auto config_iter = configs_.hwc_configs.find(config_id);
  if (config_iter == configs_.hwc_configs.end()) {
    return nullptr;
  }

  return &config_iter->second;
}

auto HwcDisplay::GetCurrentConfig() const -> const HwcDisplayConfig * {
  return GetConfig(active_config_id_);
}

auto HwcDisplay::GetLastRequestedConfig() const -> const HwcDisplayConfig * {
  return GetConfig(staged_mode_config_id_.value_or(active_config_id_));
}

const HwcDisplayConfig *HwcDisplay::GetNextConfig() const {
  if (staged_mode_config_id_ &&
      staged_mode_change_time_ <= vsync_worker_->GetNextVsyncTimestamp(
                                      ResourceManager::GetTimeMonotonicNs())) {
    return GetLastRequestedConfig();
  }
  return GetCurrentConfig();
}

bool HwcDisplay::HasPhysicalConnector() const {
  return !IsInHeadlessMode() && !is_virtual_ && pipeline_ != nullptr &&
         pipeline_->connector != nullptr &&
         pipeline_->connector->Get() != nullptr;
}

bool HwcDisplay::RefreshConfigsFromConnector() {
  if (!HasPhysicalConnector()) {
    return false;
  }

  auto *connector = pipeline_->connector->Get();
  const HwcConfigParameters params = {
      .use_color_pipeline = Properties::UseColorPipeline(),
      .persistent_hdr_enabled = Properties::PersistentHdrEnabled(),
      .external_hdr_enabled = Properties::ExternalHdrEnabled(),
      .capabilities = pipeline_->capabilities.get(),
      .min_refresh_rate = Properties::MinRefreshRate(),
      .max_refresh_rate = Properties::MaxRefreshRate(),
      .force_disable_mrr = Properties::ForceDisableMrr(),
  };
  auto configs = configs_generator_.GenerateDisplayConfigs(*connector, params);
  if (!configs) {
    return false;
  }
  configs_ = std::move(*configs);
  active_config_id_ = 0;
  ClearStagedModeConfig();
  staged_mode_config_id_ = configs_.preferred_config_id;
  link_recovery_modeset_required_ = true;
  return true;
}

bool HwcDisplay::OnLinkTrainingFailed() {
  if (!HasPhysicalConnector()) {
    return false;
  }
  auto *connector = pipeline_->connector->Get();
  if (link_recovery_modeset_required_ && !connector->GetModes().empty() &&
      !configs_.hwc_configs.empty()) {
    const auto &connector_modes = connector->GetModes();
    const bool all_connector_modes_in_configs = std::
        all_of(connector_modes.begin(), connector_modes.end(),
               [this](const auto &drm_mode) {
                 return std::any_of(configs_.hwc_configs.begin(),
                                    configs_.hwc_configs.end(),
                                    [&drm_mode](const auto &pair) {
                                      return pair.second.mode ==
                                             drm_mode.GetRawMode();
                                    });
               });
    const bool all_configs_in_connector_modes = std::
        all_of(configs_.hwc_configs.begin(), configs_.hwc_configs.end(),
               [&connector_modes](const auto &pair) {
                 return std::any_of(connector_modes.begin(),
                                    connector_modes.end(),
                                    [&pair](const auto &drm_mode) {
                                      return pair.second.mode ==
                                             drm_mode.GetRawMode();
                                    });
               });
    if (all_connector_modes_in_configs && all_configs_in_connector_modes) {
      return true;
    }
  }
  return RefreshConfigsFromConnector();
}

auto HwcDisplay::GetMaxLuminance() const -> float {
  float hdr_luminance[3]{kDefaultMaxLuminance, kDefaultMaxLuminance, 0.F};
  if (GetEdid()) {
    GetEdid()->GetHdrLuminance(&hdr_luminance[0], &hdr_luminance[1],
                               &hdr_luminance[2]);
  }
  return hdr_luminance[0] > 0.F ? hdr_luminance[0] : kDefaultMaxLuminance;
}

void HwcDisplay::SetHdrHeadroom() {
  hdr_headroom_ = GetMaxLuminance() / kHdrReferenceLuminance;
}

void HwcDisplay::SetOutputType(OutputType hdr_output_type) {
  if (!has_hdr_support_) {
    hdr_headroom_ = {};
    hdr_metadata_ = std::make_shared<hdr_output_metadata>();
    min_bpc_ = 6;
    transfer_func_ = TransferFunction::kSrgb;
    colorspace_ = forced_color_mode_
                      ? ColorUtil::ToHwcColorspace(forced_color_mode_.value())
                      : ColorUtil::ToHwcColorspace(color_mode_);
    return;
  }

  switch (hdr_output_type) {
    case OutputType::kHdr10: {
      SetHdrHeadroom();
      SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kPq);
      min_bpc_ = 8;
      colorspace_ = HwcColorspace::kBt2020;
      break;
    }
    case OutputType::kSystem: {
      std::vector<ui::Hdr> hdr_types;
      if (GetEdid()) {
        GetEdid()->GetSupportedHdrTypes(hdr_types);
      }
      if (hdr_types.empty() && !forced_color_mode_) {
        SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kSrgb);
        min_bpc_ = 6;
        colorspace_ = HwcColorspace::kBt2020;
        break;
      }

      // TODO(b/553171429): Clean up sentinel once early boot animation is active.
      if (GetBootAnimationState() == BootAnimationState::kActive) {
        hdr_headroom_ = {};
        SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kSmpte170M);
        min_bpc_ = 8;
        colorspace_ = HwcColorspace::kBt2020;
        break;
      }

      // TODO: pick appropriate HDR type
      SetHdrHeadroom();
      min_bpc_ = 8;
      colorspace_ = HwcColorspace::kBt2020;
      auto type = (hdr_types.empty() && forced_color_mode_) ? ui::Hdr::HDR10
                                                            : hdr_types.front();
      switch (type) {
        case ui::Hdr::HDR10:
          SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kPq);
          break;
        case ui::Hdr::HLG:
          SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kHlg);
          break;
        default:
          ALOGW("HDR type %d is not supported, using Display BT2020 instead.",
                static_cast<int>(type));
          SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kSmpte170M);
          break;
      }
      break;
    }
    case OutputType::kSdr:
      hdr_headroom_ = {};
      hdr_metadata_ = std::make_shared<hdr_output_metadata>();
      min_bpc_ = 6;
      transfer_func_ = TransferFunction::kSrgb;
      colorspace_ = HwcColorspace::kDefault;
      break;
    case OutputType::kInvalid:
      [[fallthrough]];
    default:
      hdr_headroom_ = {};
      hdr_metadata_ = std::make_shared<hdr_output_metadata>();
      min_bpc_ = 6;
      transfer_func_ = TransferFunction::kUnknown;
      colorspace_ = HwcColorspace::kDefault;
  }

  if (forced_color_mode_) {
    colorspace_ = ColorUtil::ToHwcColorspace(forced_color_mode_.value());
  }
}

HwcDisplay::ConfigError HwcDisplay::SetConfig(ConfigId config) {
  ATRACE_CALL();

  const HwcDisplayConfig *new_config = GetConfig(config);
  if (new_config == nullptr) {
    ALOGE("Could not find active mode for %u", config);
    return ConfigError::kBadConfig;
  }

  ALOGW_IF(staged_mode_config_id_ && *staged_mode_config_id_ != config,
           "overriding staged_mode_config_id_");
  if (active_config_id_ != 0) {
    ClearStagedModeConfig();
  }

  if (IsInHeadlessMode()) {
    active_config_id_ = config;
    ClearStagedModeConfig();
    staged_power_mode_.reset();
    link_recovery_modeset_required_ = false;
    defer_modeset_until_power_on_ = false;
    hwc_->LogRefreshRateChanges();
    return ConfigError::kNone;
  }

  // While the display's initial KMS modeset is deferred (or the display is
  // powered off), stage the client-selected mode in memory without activating
  // the CRTC in KMS until SetPowerMode(PowerMode::kOn) is called.
  if (defer_modeset_until_power_on_) {
    active_config_id_ = config;
    ClearStagedModeConfig();
    SetOutputType(new_config->output_type);
    if (vsync_worker_) {
      vsync_worker_->SetVsyncPeriodNs(new_config->mode.GetVSyncPeriodNs());
    }
    hwc_->LogRefreshRateChanges();
    return ConfigError::kNone;
  }

  ALOGV("Create modeset commit.");

  // TODO(b/533481169) - updating the state via SetOutputType should happen
  // after the successful commit via ApplyCommitChanges().
  SetOutputType(new_config->output_type);

  // Create atomic commit args for a blocking modeset. There's no need to do a
  // separate test commit, since the commit does a test anyways.
  std::optional<LayerData> modeset_layer_data = GetModesetLayerData(new_config);
  AtomicCommitArgs commit_args = CreateModesetCommit(new_config,
                                                     modeset_layer_data);
  commit_args.blocking = true;
  if (const auto result = ExecuteAtomicCommit(commit_args);
      !result.IsSuccess()) {
    ALOGE("Blocking config failed with err=%d.", result.GetStatus().error_code);
    return HwcDisplay::ConfigError::kConfigFailed;
  }

  ALOGV("Blocking config succeeded.");
  active_config_id_ = config;
  ClearStagedModeConfig();
  staged_power_mode_.reset();
  link_recovery_modeset_required_ = false;
  requires_reprobe_on_power_on_ = false;
  // CreateModesetCommit() may scan out a temporary zero-filled dumb buffer or
  // disable planes when the resolution changes. Invalidate the cached
  // composition and request a client refresh so SurfaceFlinger presents a real
  // frame for the new mode instead of leaving the temporary modeset buffer on
  // screen.
  last_presented_composition_.Reset();
  // set new vsync period
  if (vsync_worker_) {
    vsync_worker_->SetVsyncPeriodNs(new_config->mode.GetVSyncPeriodNs());
  }
  hwc_->LogRefreshRateChanges();
  hwc_->SendRefreshEventToClient(handle_);
  return ConfigError::kNone;
}

auto HwcDisplay::QueueConfig(ConfigId config, int64_t desired_time,
                             QueuedConfigTiming *out_timing) -> ConfigError {
  const HwcDisplayConfig *new_config = GetConfig(config);
  if (!new_config) {
    ALOGE("Could not find active mode for %u", config);
    return ConfigError::kBadConfig;
  }

  const HwcDisplayConfig *current_config = GetCurrentConfig();
  if (link_recovery_modeset_required_ || defer_modeset_until_power_on_ ||
      (!IsInHeadlessMode() && !GetPipe().atomic_state_manager->IsActive()) ||
      !current_config || current_config->group_id != new_config->group_id) {
    return ConfigError::kSeamlessNotAllowed;
  }

  // Estimate the timestamp of the next vsync after the desired time.
  int64_t next_vsync = vsync_worker_->GetNextVsyncTimestamp(desired_time);

  // Request a refresh from the client one vsync period before the estimated
  // timestamp.
  out_timing->refresh_time_ns = next_vsync -
                                current_config->mode.GetVSyncPeriodNs();
  out_timing->new_vsync_time_ns = next_vsync;

  // Queue the config change timing to be consistent with the requested
  // refresh time.
  staged_mode_change_time_ = out_timing->refresh_time_ns;
  staged_mode_config_id_ = config;

  if (current_config) {
    SetOutputType(current_config->output_type);
  }

  // Enable vsync events until the mode has been applied.
  vsync_worker_->SetVsyncTimestampTracking(true);

  hwc_->LogRefreshRateChanges();
  return ConfigError::kNone;
}

auto HwcDisplay::ValidateStagedComposition() -> ValidateResult {
  if (validated_composition_.has_value()) {
    ALOGE("%s: Previously validated composition was not presented", __func__);
    validated_composition_.reset();
  }

  if (IsInHeadlessMode()) {
    return {};
  }

  // Proceed if client_layer_ has a valid buffer even when layers_ is empty
  // (e.g. pure client-target composition with zero HWC layers).
  if (layers_.empty() && !client_layer_.IsLayerUsableAsDevice()) {
    ALOGV("No layers to validate.");
    return {};
  }

  /* In current drm_hwc design in case previous frame layer was not validated as
   * a CLIENT, it is used by display controller (Front buffer). We have to store
   * this state to provide the CLIENT with the release fences for such buffers.
   */
  for (auto &l : layers_) {
    l.second.SetPriorBufferScanOutFlag(l.second.GetValidatedType() !=
                                       CompositionType::kClient);
  }

  if (GetBootAnimationState() == BootAnimationState::kCleanup) {
    SetBootAnimationState(BootAnimationState::kInactive);
    if (has_hdr_support_) {
      SetOutputType(OutputType::kSystem);
    }
  }

  // Notify the flattening controller of a new frame.
  if (flatcon_) {
    if (layers_.size() <= 1) {
      flatcon_->DisableFlattening();
    } else {
      flatcon_->NewFrame();
    }
  }

  auto [composition,
        short_circuited] = pipeline_->planner->ValidateDisplay(this);
  validated_composition_.emplace(std::move(composition));
  if (!short_circuited) {
    last_presented_composition_.SetRequestedContext(
        ValidationRequestContext(*this, GetOrderLayersByZPos()));
  }

  // Iterate through the layers to find which layers actually changed.
  std::vector<ChangedLayer> changed_layers;
  for (auto &[id, layer] : layers_) {
    // Set the validated type
    auto it = validated_composition_->composition_types.find(&layer);
    ALOGE_IF(it == validated_composition_->composition_types.end(),
             "Backend did not composite layer %" PRId64 "", id);
    if (it != validated_composition_->composition_types.end()) {
      layer.SetValidatedType(it->second);
    }
    if (layer.IsTypeChanged()) {
      changed_layers.emplace_back(id, layer.GetValidatedType());
    }
  }

  // Get the layer id for the punch out layers.
  std::vector<ILayerId> punch_out_layers;
  for (const auto *layer : validated_composition_->punch_out_layers) {
    const auto it = std::find_if(layers_.begin(), layers_.end(),
                                 [&](const auto &pair) {
                                   return &pair.second == layer;
                                 });
    if (it != layers_.end()) {
      punch_out_layers.push_back(it->first);
    }
  }

  return ValidateResult{
      .changed_layers = std::move(changed_layers),
      .punch_out_layers = std::move(punch_out_layers),
  };
}

auto HwcDisplay::GetDisplayBoundsMm() const -> std::pair<int32_t, int32_t> {
  if (IsInHeadlessMode()) {
    return {configs_.mm_width, -1};
  }

  if (GetEdid()) {
    const auto bounds = GetEdid()->GetBoundsMm();
    if (bounds.first > 0 || bounds.second > 0) {
      return bounds;
    }
    if (bounds.first == 0 && bounds.second == 0) {
      if (configs_.mm_width == 0) {
        return bounds;
      }
      return {configs_.mm_width, -1};
    }
  }

  ALOGE("Failed to get display bounds for d=%d\n", int(handle_));
  // mm_width and mm_height are unreliable. so only provide mm_width to avoid
  // wrong dpi computations or other use of the values.
  return {configs_.mm_width, -1};
}

auto HwcDisplay::AcceptValidatedComposition() -> void {
  for (auto &[_, layer] : layers_) {
    layer.AcceptTypeChange();
  }
}

auto HwcDisplay::PresentStagedComposition(
    std::optional<int64_t> desired_present_time, SharedFd &out_present_fence,
    std::vector<ReleaseFence> &out_release_fences) -> bool {
  ATRACE_CALL();

  if (IsInHeadlessMode()) {
    return true;
  }

  // Commit client_layer_ when presenting a zero-layer composition that was
  // rendered directly into the client target.
  if (layers_.empty() && !client_layer_.IsLayerUsableAsDevice()) {
    ALOGV("No layers to present.");
    return true;
  }

  CompositionAttributes attributes{.display_handle = handle_};
  CompositionStats stats{};
  ++stats.total_frames;
  stats.layer_count += layers_.size();

  // With multiple displays configured at different refresh rates,
  // desired_present_time can be up to almost 2 vsync periods away for the
  // slower display. WaitLastFrame() should be called before
  // WaitForPresenttime(), otherwise  can lead to a situation where hwc sleeps
  // for up to 1.25 vsync period and blocks viable presents in SurfaceFlinger.
  GetPipe().atomic_state_manager->WaitLastFrame();

  uint32_t vperiod_ns = GetCurrentVsyncPeriodNs();
  if (desired_present_time && vperiod_ns != 0) {
    // DRM atomic uAPI does not support specifying that a commit should be
    // applied to some future vsync. Until such uAPI is available, sleep in
    // userspace until the next expected vsync time is consistent with the
    // desired present time.
    WaitForPresentTime(desired_present_time.value(), vperiod_ns);
  }

  // Check if validation was performed and update related stats. Otherwise
  // populate the composition types now.
  if (validated_composition_.has_value()) {
    attributes.validation_result = validated_composition_->flatten_reason ==
                                           FlattenReason::kValidateFailed
                                       ? ValidationResult::kFailure
                                       : ValidationResult::kSuccess;
    attributes.validation_error_code = validated_composition_->error_code;
    attributes.flatten_reason = validated_composition_->flatten_reason;
    if (validated_composition_->flatten_reason ==
        FlattenReason::kValidateFailed) {
      ++stats.failed_kms_validate;
    } else if (validated_composition_->flatten_reason != FlattenReason::kNone) {
      ++stats.frames_flattened;
    }
    if (validated_composition_->cursor_plane_validated.has_value()) {
      if (validated_composition_->cursor_plane_validated.value()) {
        ++stats.cursor_plane_frames;
      } else {
        ++stats.failed_kms_cursor_validate;
      }
    }
  } else {
    attributes.validation_result = ValidationResult::kSkip;
    validated_composition_ = CompositionPlanner::ValidatedComposition{};
    for (const auto &[id, layer] : layers_) {
      validated_composition_->composition_types
          .emplace(&layer, layer.GetValidatedType());
    }
  }

  bool has_client = false;
  for (const auto &[id, layer] : layers_) {
    stats.total_pixops += layer.GetPixOps();
    switch (layer.GetValidatedType()) {
      case CompositionType::kClient:
        has_client = true;
        stats.gpu_pixops += layer.GetPixOps();
        break;
      case CompositionType::kDevice:
      case CompositionType::kCursor:
        ++stats.used_plane_count;
        break;
      case CompositionType::kSolidColor:
        break;
      case CompositionType::kInvalid:
        ALOGE("Invalid layer type: %d",
              static_cast<int>(layer.GetValidatedType()));
        break;
      // Occlusion is achieved by dropping the layer.
      case CompositionType::kDeviceOccluded:
        break;
    }
  }

  if (has_client) {
    ++stats.used_plane_count;
  }

  const auto commit_status = CommitStagedComposition(out_present_fence);
  if (!commit_status.success) {
    attributes.present_failed = true;
    attributes.present_error_code = commit_status.error_code;
    ++stats.failed_kms_present;
    comp_stats_[attributes] += stats;
    return false;
  }

  attributes.present_failed = false;
  attributes.present_error_code = 0;
  comp_stats_[attributes] += stats;

  // Reset the hdr output metadata blobs so we don't apply it repeatedly.
  hdr_metadata_.reset();

  ++frame_no_;

  if (!out_present_fence) {
    return true;
  }

  vsync_worker_->AddLastPresentFence(out_present_fence);

  for (auto &l : layers_) {
    if (l.second.GetPriorBufferScanOutFlag()) {
      out_release_fences.emplace_back(l.first, out_present_fence);
    }
  }

  return true;
}

auto HwcDisplay::GetRawEdid() const -> std::vector<uint8_t> {
  if (IsInHeadlessMode()) {
    return {};
  }

  auto *connector = GetPipe().connector->Get();
  auto blob = connector->GetEdidBlob();
  if (!blob || blob->length == 0) {
    return {};
  }
  const uint8_t *edid_data = static_cast<uint8_t *>(blob->data);
  return {edid_data, edid_data + blob->length};
}

auto HwcDisplay::GetPort() const -> uint8_t {
  if (IsInHeadlessMode()) {
    return 0;
  }

  auto *connector = GetPipe().connector->Get();

  constexpr uint8_t kDrmDeviceBitShift = 5U;
  constexpr uint8_t kDrmDeviceBitMask = 0xE0;
  constexpr uint8_t kConnectorBitMask = 0x1F;
  const auto kDrmIdx = static_cast<uint8_t>(
      connector->GetDev().GetIndexInDevArray());
  const auto kConnectorIdx = static_cast<uint8_t>(
      connector->GetIndexInResArray());
  return (((kDrmIdx << kDrmDeviceBitShift) & kDrmDeviceBitMask) |
          (kConnectorIdx & kConnectorBitMask));
}

auto HwcDisplay::GetDisplayType() const -> DisplayType {
  if (is_virtual_) {
    return kVirtual;
  }

  if (IsInHeadlessMode()) {
    return kInternal;
  }

  /* Primary display should be always internal,
   * otherwise SF will be unhappy and will crash
   */
  if (handle_ == kPrimaryDisplay) {
    return kInternal;
  }

  auto displays = hwc_->GetResMan().GetInternalDisplayNames();
  if (!displays.empty()) {
    std::string name = GetPipe().connector->Get()->GetName();
    const bool is_internal = (displays.find(name) != displays.end());
    return is_internal ? kInternal : kExternal;
  }

  if (GetPipe().connector->Get()->IsInternal())
    return kInternal;

  ALOGW_IF(!GetPipe().connector->Get()->IsExternal(),
           "Connector type is neither internal nor external.");
  return kExternal;
}

void HwcDisplay::SetVsyncCallbacksEnabled(bool enabled) {
  // Enabling vsync callbacks for a virtual display succeeds with no effect.
  if (!vsync_worker_) {
    ALOGE_IF(!is_virtual_, "Invalid VSyncWorker. Did HwcDisplay::Init fail?");
    return;
  }

  vsync_event_en_ = enabled;
  std::optional<VSyncWorker::VsyncTimestampCallback> callback = std::nullopt;
  if (vsync_event_en_) {
    DrmHwc *hwc = hwc_;
    DisplayHandle id = handle_;
    // Callback will be called from the vsync thread.
    callback = [hwc, id](int64_t timestamp, uint32_t period_ns) {
      hwc->SendVsyncEventToClient(id, timestamp, period_ns);
    };
  }
  vsync_worker_->SetTimestampCallback(std::move(callback));
}

bool HwcDisplay::IsVsyncTimestampTrackingEnabled() const {
  return vsync_worker_ != nullptr &&
         vsync_worker_->IsVsyncTimestampTrackingEnabled();
}

void HwcDisplay::ClearStagedModeConfig() {
  staged_mode_config_id_.reset();
  staged_mode_change_time_ = 0;
  if (vsync_worker_) {
    vsync_worker_->SetVsyncTimestampTracking(false);
  }
}

bool HwcDisplay::IsDozeSupported() const {
  if (IsInHeadlessMode()) {
    return false;
  }
  return GetPipe().device->GetBackend().SupportsDoze();
}

bool HwcDisplay::IsDozeSuspendSupported() const {
  if (IsInHeadlessMode()) {
    return false;
  }
  return GetPipe().device->GetBackend().SupportsDozeSuspend();
}

bool HwcDisplay::IsSuspendSupported() const {
  if (IsInHeadlessMode()) {
    return false;
  }
  return GetPipe().device->GetBackend().SupportsSuspend();
}

HwcDisplay::Error HwcDisplay::SetPowerMode(PowerMode mode) {
  // Check support before headless because VTS expects headless mode to not
  // support these.
  if (mode == PowerMode::kDoze && !IsDozeSupported()) {
    return HwcDisplay::Error::kUnsupported;
  }
  if (mode == PowerMode::kDozeSuspend && !IsDozeSuspendSupported()) {
    return HwcDisplay::Error::kUnsupported;
  }
  if (mode == PowerMode::kSuspend && !IsSuspendSupported()) {
    return HwcDisplay::Error::kUnsupported;
  }

  if (IsInHeadlessMode()) {
    staged_power_mode_.reset();
    return HwcDisplay::Error::kNone;
  }
  bool enabled = mode != PowerMode::kOff;

  // If the request is to enable the display, the CRTC is not active, and an
  // active config is set, try to reconfigure the pipeline with SetConfig.
  if (enabled) {
    defer_modeset_until_power_on_ = false;
    if (GetPipe().atomic_state_manager->IsActive() &&
        GetCurrentConfig() != nullptr) {
      if (vsync_event_en_) {
        SetVsyncCallbacksEnabled(true);
      }
      staged_power_mode_.reset();
      return HwcDisplay::Error::kNone;
    }

    const bool
        has_external_connector = HasPhysicalConnector() &&
                                 GetPipe().connector->Get()->IsExternal();
    if (has_external_connector) {
      auto *connector = GetPipe().connector->Get();
      if (requires_reprobe_on_power_on_ || connector->GetModes().empty()) {
        connector->UpdateModesAndProperties();
      }
      if (!connector->IsConnected()) {
        ALOGW("Cannot enable external display on disconnected connector %u",
              connector->GetId());
        staged_power_mode_ = mode;
        return HwcDisplay::Error::kBadParameter;
      }
    }

    // Re-register the VSync timestamp callback if VSync delivery was enabled
    // prior to powering off the display.
    if (vsync_event_en_) {
      SetVsyncCallbacksEnabled(true);
    }

    const HwcDisplayConfig *last_requested_config = GetLastRequestedConfig();
    if (last_requested_config) {
      if (SetConfig(last_requested_config->id) != ConfigError::kNone) {
        ALOGE("Failed to set config to re-enable display after teardown.");
        staged_power_mode_ = mode;
        return HwcDisplay::Error::kBadParameter;
      }
      // SetConfig() executes a blocking modeset commit with PowerMode::kOn,
      // which already activates the CRTC in KMS (unless the display is in
      // headless mode). Return early to avoid issuing a redundant empty atomic
      // commit below.
      if (GetPipe().atomic_state_manager->IsActive()) {
        return HwcDisplay::Error::kNone;
      }
    }
  } else {
    staged_power_mode_.reset();
    // Clear the VSyncWorker callback and disable timestamp tracking directly so
    // VSync events stop firing while the display is powered off, while
    // preserving vsync_event_en_ and staged_mode_config_id_ for when the
    // display is powered back on.
    if (vsync_worker_) {
      vsync_worker_->SetTimestampCallback(std::nullopt);
      vsync_worker_->SetVsyncTimestampTracking(false);
    }
    const bool has_kernel_crtc = HasPhysicalConnector() &&
                                 GetPipe()
                                         .connector->Get()
                                         ->GetCrtcIdProperty()
                                         .GetValue()
                                         .value_or(0) != 0;
    if (!GetPipe().atomic_state_manager->IsActive() && !has_kernel_crtc) {
      defer_modeset_until_power_on_ = true;
      last_presented_composition_.Reset();
      return HwcDisplay::Error::kNone;
    }
    requires_reprobe_on_power_on_ = true;
  }

  // Set the display active state.
  AtomicCommitArgs a_args{};
  a_args.blocking = true;
  a_args.power_mode = mode;
  if (!enabled) {
    a_args.teardown = true;
  }

  const auto result = ExecuteAtomicCommit(a_args);
  ALOGE_IF(!result.IsSuccess(), "Failed to set display active: %s. err=%d",
           enabled ? "enabled" : "disabled", result.GetStatus().error_code);
  // If setting to |enabled|, log the error and stage the power mode so the
  // next frame update retries activating the display.
  if (!result.IsSuccess()) {
    if (!enabled) {
      return HwcDisplay::Error::kBadParameter;
    }
    staged_power_mode_ = mode;
    return HwcDisplay::Error::kNone;
  }
  staged_power_mode_.reset();
  if (enabled) {
    requires_reprobe_on_power_on_ = false;
  } else {
    defer_modeset_until_power_on_ = true;
    last_presented_composition_.Reset();
  }
  return HwcDisplay::Error::kNone;
}

bool HwcDisplay::GetDisplayEnabled() const {
  if (IsInHeadlessMode()) {
    return true;
  }

  return GetPipe().atomic_state_manager->IsActive();
}

bool HwcDisplay::SetPipeline(std::shared_ptr<DrmDisplayPipeline> pipeline) {
  if (pipeline_ == nullptr || pipeline == nullptr) {
    staged_mode_config_id_.reset();
    active_config_id_ = 0;
  }
  Deinit();

  pipeline_ = std::move(pipeline);

  if (pipeline_ != nullptr || handle_ == kPrimaryDisplay) {
    bool success = Init();
    if (!success) {
      ALOGE("Failed to init HwcDisplay after setting pipeline.");
      // Clean up partial state and detach the failed pipeline. External
      // displays remain unbound so ResourceManager can retry on a later hotplug
      // uevent (e.g. after a transient 0-mode EDID/AUX read failure resolves),
      // whereas kPrimaryDisplay must re-initialize in headless mode so
      // SurfaceFlinger always has an active primary display.
      Deinit();
      pipeline_.reset();
      if (handle_ == kPrimaryDisplay) {
        ALOGI(
            "Re-initializing primary display in headless mode after pipeline "
            "init failure.");
        Init();
        hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kConnected);
      }
      return false;
    }
    hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kConnected);
    if (pipeline_) {
      LogModesOnHotplug();
    }
    return true;
  }

  hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kDisconnected);
  return true;
}

void HwcDisplay::Deinit() {
  if (pipeline_ != nullptr) {
    if (pipeline_->connector && pipeline_->connector->Get()) {
      auto *connector = pipeline_->connector->Get();
      connector->GetDev()
          .GetBackend()
          .SetRefreshCallbackForConnector(connector->GetId(), nullptr);
    }

    // Pass an empty composition plan alongside power_mode = kOff and
    // teardown = true so SetCompositionIfNeeded disables all previously used
    // planes in the same blocking commit that tears down the CRTC and
    // connector.
    AtomicCommitArgs a_args{};
    a_args.blocking = true;
    a_args.composition = std::make_shared<LayerToPlaneJoiningPlan>();
    a_args.power_mode = PowerMode::kOff;
    a_args.teardown = true;
    ExecuteAtomicCommit(a_args);

    validated_composition_.reset();
    ClearStagedModeConfig();
    staged_power_mode_.reset();
    flatcon_.reset();
    hdcpcon_.reset();
    backlight_controller_.reset();
    last_presented_composition_.Reset();
  }
  edid_wrapper_.reset();
  link_recovery_modeset_required_ = false;
  defer_modeset_until_power_on_ = false;

  if (vsync_worker_) {
    vsync_worker_->StopThread();
    vsync_worker_ = {};
  }
}

void HwcDisplay::InitForcedColorMode() {
  if (IsInHeadlessMode()) {
    return;
  }

  // Forced color modes only apply to the internal display
  if (GetPipe().connector && GetPipe().connector->Get() &&
      GetPipe().connector->Get()->IsExternal()) {
    return;
  }

  if (hwc_->GetResMan().ForceColorMode() < 0) {
    return;
  }

  auto force_color_mode = static_cast<ColorMode>(
      hwc_->GetResMan().ForceColorMode());
  if (force_color_mode < ColorMode::kNative ||
      force_color_mode > ColorMode::kDisplayBt2020) {
    return;
  }

  forced_color_mode_ = force_color_mode;
}

void HwcDisplay::InitUseColorPipeline() {
  if (IsInHeadlessMode()) {
    use_color_pipeline_ = false;
    return;
  }

  use_color_pipeline_ = hwc_->GetResMan().UseColorPipeline() &&
                        GetPipe().primary_plane &&
                        GetPipe().primary_plane->Get() &&
                        GetPipe().primary_plane->Get()->HasColorPipeline();
}

void HwcDisplay::InitWcgSupported() {
  if (IsInHeadlessMode()) {
    has_wcg_support_ = false;
    return;
  }

  bool crtc_ctm = GetPipe().crtc && GetPipe().crtc->Get() &&
                  GetPipe().crtc->Get()->GetCtmProperty();
  std::vector<ColorMode> color_modes;
  if (GetEdid()) {
    GetEdid()->GetColorModes(color_modes);
  }
  // TODO check for WCG modes
  has_wcg_support_ = (use_color_pipeline_ || crtc_ctm) &&
                     (!color_modes.empty() ||
                      (forced_color_mode_ &&
                       forced_color_mode_.value() != ColorMode::kNative));
}

void HwcDisplay::InitHdrSupported() {
  if (IsInHeadlessMode()) {
    has_hdr_support_ = false;
    return;
  }

  if (!GetPipe().connector || !GetPipe().connector->Get()) {
    has_hdr_support_ = false;
    return;
  }

  const bool hdr_sysprop_enabled = GetPipe().connector->Get()->IsExternal()
                                       ? hwc_->GetResMan().ExternalHdrEnabled()
                                       : hwc_->GetResMan()
                                             .PersistentHdrEnabled();
  if (!hdr_sysprop_enabled) {
    has_hdr_support_ = false;
    return;
  }

  if (pipeline_->capabilities) {
    auto override_types = pipeline_->capabilities->GetHdrTypesOverride();
    if (override_types.has_value()) {
      has_hdr_support_ = !override_types->empty();
      ALOGI("InitHdrSupported: using backend override: has_hdr_support_=%d",
            has_hdr_support_);
      return;
    }
  }

  bool crtc_gamma = GetPipe().crtc && GetPipe().crtc->Get() &&
                    GetPipe().crtc->Get()->GetGammaLutProperty() &&
                    GetPipe().crtc->Get()->GetGammaLutSizeProperty();
  std::vector<ui::Hdr> hdr_types;
  if (GetEdid()) {
    GetEdid()->GetSupportedHdrTypes(hdr_types);
  }

  // TODO check for HDR types
  has_hdr_support_ = use_color_pipeline_ && crtc_gamma && has_wcg_support_ &&
                     GetPipe()
                         .connector->Get()
                         ->GetHdrOutputMetadataProperty() &&
                     GetPipe().connector->Get()->GetColorspaceProperty() &&
                     (!hdr_types.empty() ||
                      (forced_color_mode_ &&
                       forced_color_mode_.value() != ColorMode::kNative));
}

bool HwcDisplay::Init() {
  if (!is_virtual_) {
    vsync_worker_ = VSyncWorker::CreateInstance(pipeline_);
    if (!vsync_worker_) {
      ALOGE("Failed to create event worker for d=%d\n", int(handle_));
      return false;
    }
    if (vsync_event_en_) {
      SetVsyncCallbacksEnabled(true);
    }
  }

  requires_reprobe_on_power_on_ = false;
  defer_modeset_until_power_on_ = false;
  if (!IsInHeadlessMode()) {
    if (Properties::FlatteningEnabled()) {
      auto flatcbk = (struct FlatConCallbacks){
          .trigger = [this]() { hwc_->SendRefreshEventToClient(handle_); }};
      flatcon_ = std::make_unique<FlatteningController>(handle_, flatcbk,
                                                        kFlatteningTimeout);
    }

    if (pipeline_ && pipeline_->connector && pipeline_->connector->Get()) {
      auto *connector = pipeline_->connector->Get();
      // Probe the connector only if it has not already been populated by
      // ResourceManager::UpdateFrontendDisplays(). Doing this before reading
      // the EDID blob or HDCP properties also ensures direct SetPipeline()
      // callers observe fresh connector state without issuing a redundant
      // second drmModeGetConnector() ioctl during hotplug.
      if (connector->GetModes().empty()) {
        int ret = connector->UpdateModesAndProperties();
        if (ret != 0) {
          ALOGE("Failed to update display modes with error: %d", ret);
          return false;
        }
      }
      connector->GetDev()
          .GetBackend()
          .SetRefreshCallbackForConnector(connector->GetId(), [this]() {
            hwc_->SendRefreshEventToClient(handle_);
          });
    }

    if (IsHdcpPropertyPresent()) {
      ALOGI("HDCP properties found on display %d", int(handle_));
      auto hdcpconcbks = (struct HdcpConCallbacks){
          .notify_hdcp_status =
              [this](std::optional<HdcpContentType> level) {
                hwc_->SendHdcpLevelsChangedEventToClient(handle_, level);
              },
          .trigger_retry_frame =
              [this]() { hwc_->SendRefreshEventToClient(handle_); }};
      hdcpcon_ = std::make_unique<HdcpController>(&GetPipe(), hdcpconcbks,
                                                  kHdcpRetryTimeout);
    }

#if HAS_LIBDISPLAY_INFO
    edid_wrapper_ = LibdisplayEdidWrapper::Create(
        pipeline_->connector->Get()->GetEdidBlob());
    if (!edid_wrapper_) {
      ALOGW("Failed to create a LibdisplayInfo parser.");
    }
#endif

    // Attempt to initialize backlight
    if (GetPipe().connector->Get()->IsInternal()) {
      auto backlights = SysfsBacklightController::EnumerateBacklights();
      for (const auto &name : backlights) {
        // TODO(seanpaul): logic to associate backlight with connector
        backlight_controller_ = SysfsBacklightController::
            CreateInstanceFromName(name);
        if (backlight_controller_) {
          ALOGI("Associated backlight %s with display %d", name.c_str(),
                static_cast<int>(handle_));
          break;
        }
      }
    }
  }

  HwcLayer::LayerProperties lp;
  lp.blend_mode = BufferBlendMode::kPreMult;
  client_layer_.SetLayerProperties(lp);

  std::optional<HwcDisplayConfig> prev_config;
  if (const auto *last_req = GetLastRequestedConfig(); last_req != nullptr) {
    prev_config = *last_req;
  }

  if (is_virtual_) {
    configs_ = configs_generator_.GetFakeMode(virtual_disp_width_,
                                              virtual_disp_height_);
    pipeline_->writeback_connector = pipeline_->connector;
  } else if (IsInHeadlessMode()) {
    configs_ = configs_generator_.GetFakeMode(0, 0);
  } else {
    // Ensure one config is available for headless mode in case we end up with
    // no real modes from the connector or if initialization fails.
    configs_ = configs_generator_.GetFakeMode(0, 0);
    active_config_id_ = configs_.preferred_config_id;

    if (!RefreshConfigsFromConnector()) {
      return false;
    }
    active_config_id_ = configs_.preferred_config_id;
    ClearStagedModeConfig();
    link_recovery_modeset_required_ = false;
  }

  InitForcedColorMode();
  InitUseColorPipeline();
  InitWcgSupported();
  InitHdrSupported();

  auto boot_state = BootAnimationState::kInactive;
  if (!IsInHeadlessMode() && !pipeline_->connector->Get()->IsExternal()) {
    if (Properties::EarlyBootAnimationEnabled()) {
      if (hwc_ != nullptr && hwc_->InitEarlyBootAnimation(this)) {
        boot_state = BootAnimationState::kActive;
      }
    } else if (!Properties::BootAnimationCompleted()) {
      boot_state = BootAnimationState::kActive;
    }
  }
  SetBootAnimationState(boot_state);

  // Some backend displays (such as SDR external monitors on Qualcomm SDM) only
  // advertise ColorMode::kNative in GetColorModeOverrides() and reject kSrgb.
  ColorMode default_color_mode = ColorMode::kSrgb;
  if (!IsInHeadlessMode() && GetPipe().capabilities != nullptr) {
    if (auto overrides = GetPipe().capabilities->GetColorModeOverrides();
        overrides && !overrides->empty() &&
        std::find(overrides->begin(), overrides->end(), ColorMode::kSrgb) ==
            overrides->end()) {
      default_color_mode = std::find(overrides->begin(), overrides->end(),
                                     ColorMode::kNative) != overrides->end()
                               ? ColorMode::kNative
                               : overrides->front();
    }
  }
  SetColorMode(default_color_mode, ui::RenderIntent::COLORIMETRIC);

  ConfigId initial_config_id = configs_.preferred_config_id;
  if (prev_config.has_value()) {
    constexpr float kRefreshRateEpsilon = 0.5F;
    for (const auto &[id, cfg] : configs_.hwc_configs) {
      if (cfg.mode.GetRawMode().hdisplay ==
              prev_config->mode.GetRawMode().hdisplay &&
          cfg.mode.GetRawMode().vdisplay ==
              prev_config->mode.GetRawMode().vdisplay &&
          std::abs(cfg.mode.GetVRefresh() - prev_config->mode.GetVRefresh()) <
              kRefreshRateEpsilon &&
          cfg.output_type == prev_config->output_type) {
        initial_config_id = id;
        break;
      }
    }
  }

  if (GetDisplayType() == DisplayType::kExternal && !prev_config.has_value()) {
    // Defer the initial blocking KMS modeset on external displays until the
    // client (SurfaceFlinger / DisplayManager) finishes selecting its target
    // mode and calls SetPowerMode(PowerMode::kOn). This avoids holding
    // GetMainLock() on UEventThread across a blocking modeset, eliminates the
    // ON -> OFF -> ON triple-modeset during SurfaceFlinger display
    // registration, and avoids forcing the connector's raw preferred mode
    // before DisplayManager applies its multi-display topology policy.
    if (const HwcDisplayConfig *preferred_config = GetConfig(
            configs_.preferred_config_id);
        preferred_config != nullptr) {
      SetOutputType(preferred_config->output_type);
      vsync_worker_->SetVsyncPeriodNs(
          preferred_config->mode.GetVSyncPeriodNs());
    }
    defer_modeset_until_power_on_ = true;
    staged_power_mode_ = PowerMode::kOn;
  } else {
    auto set_config_err = SetConfig(initial_config_id);
    if (set_config_err != HwcDisplay::ConfigError::kNone &&
        initial_config_id != configs_.preferred_config_id) {
      set_config_err = SetConfig(configs_.preferred_config_id);
    }
    if (set_config_err != HwcDisplay::ConfigError::kNone) {
      ALOGW(
          "Failed to set preferred config %u during Init on display %d; "
          "keeping display initialized for subsequent userspace modesets.",
          configs_.preferred_config_id, static_cast<int>(handle_));
      staged_power_mode_ = PowerMode::kOn;
      if (boot_state == BootAnimationState::kActive) {
        SetBootAnimationState(BootAnimationState::kInactive);
        if (hwc_ != nullptr) {
          hwc_->AbortEarlyBootAnimation();
        }
      }
    }
  }

  if (!IsInHeadlessMode() && GetPipe().connector->Get()->IsInternal()) {
    SetConfigGroupsForActiveConfig();
  }

  return true;
}

std::optional<PanelOrientation> HwcDisplay::getDisplayPhysicalOrientation()
    const {
  if (IsInHeadlessMode()) {
    // The pipeline can be nullptr in headless mode, so return the default
    // "normal" mode.
    return PanelOrientation::kModePanelOrientationNormal;
  }

  const DrmDisplayPipeline &pipeline = GetPipe();
  if (pipeline.connector == nullptr || pipeline.connector->Get() == nullptr) {
    ALOGW(
        "No display pipeline present to query the panel orientation property.");
    return {};
  }

  return pipeline.connector->Get()->GetPanelOrientation();
}

auto HwcDisplay::CreateLayer(ILayerId new_layer_id) -> bool {
  if (layers_.count(new_layer_id) > 0)
    return false;

  layers_.emplace(new_layer_id, HwcLayer(this));

  return true;
}

auto HwcDisplay::DestroyLayer(ILayerId layer_id) -> bool {
  // TODO(b/553171429): Clean up sentinel once early boot animation is active.
  // BootAnimation creates a single surface for its entire lifecycle and drops
  // it on exit. Use the first layer destruction as the completion signal.
  if (GetBootAnimationState() == BootAnimationState::kActive &&
      !Properties::EarlyBootAnimationEnabled()) {
    SetBootAnimationState(BootAnimationState::kCleanup);
  }
  auto count = layers_.erase(layer_id);
  return count != 0;
}

auto HwcDisplay::GetColorModes() const -> std::vector<ColorMode> {
  if (IsInHeadlessMode()) {
    return {ColorMode::kNative};
  }

  // Return the backend overrides if they exist.
  if (GetPipe().capabilities != nullptr) {
    auto color_mode_overrides = GetPipe().capabilities->GetColorModeOverrides();
    if (color_mode_overrides) {
      ALOGI("Using backend ColorMode overrides");
      return color_mode_overrides.value();
    }
  }

  // If forced_color_mode_ is set, override the color modes.
  if (forced_color_mode_.has_value()) {
    std::set<ColorMode> modes;
    modes.emplace(ColorMode::kNative);
    modes.emplace(ColorMode::kSrgb);
    modes.emplace(forced_color_mode_.value());
    return {modes.begin(), modes.end()};
  }

  if (!GetPipe().connector->Get()->GetColorspaceProperty()) {
    return {ColorMode::kNative};
  }

  // TODO: refactor to use set instead of vector
  std::vector<ColorMode> modes;
  if (GetEdid()) {
    GetEdid()->GetColorModes(modes);
  }

  if (modes.empty()) {
    return {ColorMode::kNative};
  }

  modes.emplace_back(ColorMode::kSrgb);
  return modes;
}

auto HwcDisplay::GetRenderIntents(ColorMode /*color_mode*/) const
    -> std::vector<ui::RenderIntent> {
  return {ui::RenderIntent::COLORIMETRIC,
          ui::RenderIntent::TONE_MAP_COLORIMETRIC, kVendorBoostedRenderIntent};
}

void HwcDisplay::SetColorMode(ColorMode mode, ui::RenderIntent render_intent) {
  color_mode_ = mode;

  switch (render_intent) {
    case ui::RenderIntent::COLORIMETRIC:
    case ui::RenderIntent::TONE_MAP_COLORIMETRIC:
      render_intent_matrix_ = GetIdentityCtmPtr();
      break;
    default:
      if (render_intent == kVendorBoostedRenderIntent) {
        render_intent_matrix_ = GetBoostedCTMPtr();
      } else {
        ALOGW("Unsupported render intent (%d). Fallback to identity CTM",
              static_cast<int32_t>(render_intent));
        render_intent_matrix_ = GetIdentityCtmPtr();
      }
  }

  UpdateColorTransformMatrix();
}

void HwcDisplay::GetHdrCapabilities(std::vector<ui::Hdr> *types,
                                    float *max_luminance,
                                    float *max_average_luminance,
                                    float *min_luminance) const {
  if (IsInHeadlessMode() || !has_hdr_support_) {
    return;
  }

  if (pipeline_->capabilities) {
    auto override_types = pipeline_->capabilities->GetHdrTypesOverride();
    if (override_types.has_value()) {
      *types = *override_types;
      if (GetEdid()) {
        GetEdid()->GetHdrLuminance(max_luminance, max_average_luminance,
                                   min_luminance);
      }
      return;
    }
  }

  types->clear();
  if (GetEdid()) {
    GetEdid()->GetHdrCapabilities(*types, max_luminance, max_average_luminance,
                                  min_luminance);
  }
  // TODO: pick appropriate HDR type
  if (types->empty() && forced_color_mode_) {
    types->emplace_back(ui::Hdr::HDR10);
  }
}

auto HwcDisplay::IsHdcpPropertyPresent() -> bool {
  if (IsInHeadlessMode()) {
    return false;
  }
  if (!GetPipe().connector->Get()->GetContentProtectionProperty() ||
      !GetPipe().connector->Get()->GetHdcpContentTypeProperty()) {
    return false;
  }
  return true;
}

auto HwcDisplay::StartHdcp() -> bool {
  if (hdcpcon_ == nullptr) {
    const int forced_level = Properties::ForceHdcpLevel();
    if (forced_level == 0) {
      ALOGI("HDCP requested to start (forced kType0)");
      hwc_->SendHdcpLevelsChangedEventToClient(handle_,
                                               HdcpContentType::kType0);
      return true;
    }
    if (forced_level == 1) {
      ALOGI("HDCP requested to start (forced kType1)");
      hwc_->SendHdcpLevelsChangedEventToClient(handle_,
                                               HdcpContentType::kType1);
      return true;
    }
    // Virtual emulators lack physical HDCP hardware. Advertise HDCP_V1
    // so secure content and lockscreen bouncers render on external displays.
    if (Properties::IsEmulator()) {
      hwc_->SendHdcpLevelsChangedEventToClient(handle_,
                                               HdcpContentType::kType0);
      return true;
    }
    ALOGE(
        "HDCP requested, but HDCP properties not available on that "
        "display");
    return false;
  }
  hdcpcon_->Start();
  return true;
}

auto HwcDisplay::StopHdcp() -> bool {
  /*
   * Client or Kernel can Terminate Hdcp
   * If Client or kernel requests to terminate hdcp, internal state is set to
   * Undesired Since the HDCP Content and Content Protection prop are optional
   * We need to make sure the connector has these properties else
   * return a false to indicate that the request to start/stop
   * HDCP cannot be completed.
   */
  if (hdcpcon_ == nullptr) {
    ALOGE(
        "Client requested HDCP, but HDCP properties not available on that "
        "display");
    return false;
  }
  // Client or Kernel requested HDCP termination. Only act if HDCP is currently
  // enabled.
  if (hdcpcon_ &&
      hdcpcon_->GetHdcpState() == HdcpController::HdcpState::kEnabled) {
    ALOGI(" Client or Kernel requested to terminate HDCP");
    hdcpcon_->Terminate();
  }
  return true;
}

std::optional<AtomicCommitArgs> HwcDisplay::CreateModesetCommit(
    ConfigId configId) {
  if (IsInHeadlessMode()) {
    return std::nullopt;
  }

  const auto *config = GetConfig(configId);
  if (config == nullptr) {
    ALOGE("Could not find active mode for %u", configId);
    return std::nullopt;
  }

  // TODO(b/533481169) - this function should be const and the state of
  // HwcDisplay should only be change after a successful commit.
  SetOutputType(config->output_type);
  std::optional<LayerData> modeset_layer_data = GetModesetLayerData(config);
  return CreateModesetCommit(config, modeset_layer_data);
}

AtomicCommitArgs HwcDisplay::CreateModesetCommit(
    const HwcDisplayConfig *config,
    const std::optional<LayerData> &modeset_layer) {
  AtomicCommitArgs args{};

  args.brightness = brightness_;
  args.brightness_nits = brightness_nits_;
  args.color_matrix = color_matrix_;
  args.content_type = content_type_;
  args.colorspace = colorspace_;
  args.transfer_func = transfer_func_;
  args.hdr_metadata = hdr_metadata_;
  args.min_bpc = min_bpc_;
  args.hdr_headroom = hdr_headroom_;
  args.color_mode = color_mode_;

  std::vector<LayerData> composition_layers;
  if (modeset_layer) {
    composition_layers.emplace_back(modeset_layer.value());
  }

  if (composition_layers.empty()) {
    ALOGW("Attempting to create a modeset commit without a layer.");
  }

  args.display_mode = config->mode;
  args.display_config_id = config->id;
  args.power_mode = PowerMode::kOn;
  args.composition = LayerToPlaneJoiningPlan::
      CreateLayerToPlaneJoiningPlan(GetPipe(), std::move(composition_layers));
  ALOGW_IF(!args.composition, "No composition for blocking modeset");

  return args;
}

CommitStatusOr<AtomicCommitResult> HwcDisplay::ExecuteAtomicCommit(
    AtomicCommitArgs &a_args) const {
  const int64_t commit_start_time = ResourceManager::GetTimeMonotonicNs();
  const auto status_or_result = GetPipe()
                                    .device->GetAtomicCommitSink()
                                    .ExecuteAtomicCommit(
                                        {{GetPipe().atomic_state_manager.get(),
                                          a_args}});
  const int64_t commit_end_time = ResourceManager::GetTimeMonotonicNs();

  // Log successful modesets (seamless and full), including teardowns.
  const bool is_config_change = a_args.display_mode || a_args.power_mode ||
                                a_args.teardown;
  if (is_config_change) {
    LogConfigResult(a_args, status_or_result.IsSuccess(),
                    commit_end_time - commit_start_time);
  }

  const auto &res = status_or_result.GetValue();
  if (!res.has_value()) {
    return CommitStatusOr<AtomicCommitResult>(status_or_result.GetStatus());
  }

  ALOGE_IF(res->size() > 1,
           "More than one result returned for a singular display");
  if (res->empty()) {
    return CommitStatusOr<AtomicCommitResult>(CommitStatus::InternalFailure());
  }
  if (res->front().first != GetPipe().atomic_state_manager.get()) {
    ALOGE("Results are for a different state manager.");
    return CommitStatusOr<AtomicCommitResult>(CommitStatus::InternalFailure());
  }
  return CommitStatusOr<AtomicCommitResult>(res->front().second);
}

void HwcDisplay::WaitForPresentTime(int64_t present_time,
                                    uint32_t vsync_period_ns) {
  const int64_t current_time = ResourceManager::GetTimeMonotonicNs();
  int64_t next_vsync_time = vsync_worker_->GetNextVsyncTimestamp(current_time);

  int64_t vsync_after_present_time = vsync_worker_->GetNextVsyncTimestamp(
      present_time);
  int64_t vsync_before_present_time = vsync_after_present_time -
                                      vsync_period_ns;

  // Check if |present_time| is closer to the expected vsync before or after.
  int64_t desired_vsync = (vsync_after_present_time - present_time) <
                                  (present_time - vsync_before_present_time)
                              ? vsync_after_present_time
                              : vsync_before_present_time;

  // Don't sleep if desired_vsync is before or nearly equal to vsync_period of
  // the next expected vsync.
  const int64_t quarter_vsync_period = vsync_period_ns / 4;
  if ((desired_vsync - next_vsync_time) < quarter_vsync_period) {
    return;
  }

  // Sleep until 75% vsync_period before the desired_vsync.
  int64_t sleep_until = desired_vsync - (quarter_vsync_period * 3);

  ATRACE_NAME("WaitForPresentTime");

  // NOLINTBEGIN
  std::stringstream oss;
  oss << "current_time: " << current_time
      << " next_vsync_time: " << next_vsync_time << " (rel "
      << ((next_vsync_time - current_time) / 1000000.00) << "ms)"
      << " desired_vsync: " << desired_vsync << " (rel "
      << ((desired_vsync - current_time) / 1000000.00) << "ms)"
      << " vsync_period_ns: " << vsync_period_ns
      << " sleep_until: " << sleep_until << " (rel "
      << ((sleep_until - current_time) / 1000000.00) << "ms)";
  ATRACE_INSTANT(oss.str().c_str());
  // NOLINTEND

  struct timespec sleep_until_ts{};
  constexpr int64_t kOneSecondNs = 1LL * 1000 * 1000 * 1000;
  sleep_until_ts.tv_sec = int(sleep_until / kOneSecondNs);
  sleep_until_ts.tv_nsec = int(sleep_until -
                               (sleep_until_ts.tv_sec * kOneSecondNs));
  // NOLINTNEXTLINE(misc-include-cleaner)
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_until_ts, nullptr);
}

uint32_t HwcDisplay::GetCurrentVsyncPeriodNs() const {
  const HwcDisplayConfig *config = GetCurrentConfig();
  if (config == nullptr) {
    return 0;
  }
  return config->mode.GetVSyncPeriodNs();
}

CommitStatus HwcDisplay::TestComposition(
    CompositionPlanner::ValidatedComposition &composition) const {
  ATRACE_CALL();

  if (IsInHeadlessMode()) {
    return CommitStatus::Success();
  }

  PrepareCompositionForCommit(composition);
  auto a_args = CreateFrameUpdateCommit(composition);
  if (!a_args) {
    return CommitStatus::InternalFailure();
  }

  const auto result = GetPipe().device->GetAtomicCommitSink().TestAtomicCommit(
      {{GetPipe().atomic_state_manager.get(), *a_args}});
  if (result.success) {
    // Put the composition plan into the newly-validated composition. Its owner
    // is responsible for keeping it alive until commit.
    composition.composition_plan = a_args->composition;
  }

  return result;
}

void HwcDisplay::PrepareCompositionForCommit(
    CompositionPlanner::ValidatedComposition &composition) const {
  UpdateClientTargetIfNeeded(client_layer_, composition.composition_plan.get());
}

std::optional<AtomicCommitArgs> HwcDisplay::CreateFrameUpdateCommit(
    const CompositionPlanner::ValidatedComposition &composition) const {
  if (IsInHeadlessMode()) {
    ALOGE("%s: Display is in headless mode, should never reach here", __func__);
    return AtomicCommitArgs{};
  }

  AtomicCommitArgs a_args;
  a_args.brightness = brightness_;
  a_args.brightness_nits = brightness_nits_;
  a_args.color_matrix = color_matrix_;
  a_args.content_type = content_type_;
  a_args.colorspace = colorspace_;
  a_args.transfer_func = transfer_func_;
  a_args.hdr_metadata = hdr_metadata_;
  a_args.min_bpc = min_bpc_;
  a_args.hdr_headroom = hdr_headroom_;
  a_args.color_mode = color_mode_;

  if (!link_recovery_modeset_required_ && active_config_id_ != 0 &&
      staged_mode_config_id_ &&
      staged_mode_change_time_ <= ResourceManager::GetTimeMonotonicNs()) {
    const auto *staged_config = GetConfig(staged_mode_config_id_.value());
    if (staged_config == nullptr) {
      return std::nullopt;
    }

    a_args.display_mode = staged_config->mode;
    a_args.display_config_id = staged_config->id;
    a_args.seamless = true;
  }

  // If enabling the display failed in Init() or SetPowerMode(),
  // staged_power_mode_ holds the pending power mode so the next frame update
  // retries activating the CRTC as a blocking full modeset with a valid
  // display_mode.
  if (staged_power_mode_) {
    if (!a_args.display_mode) {
      const auto *target_config = GetLastRequestedConfig();
      if (target_config == nullptr) {
        return std::nullopt;
      }
      a_args.display_mode = target_config->mode;
      a_args.display_config_id = target_config->id;
    }
    a_args.power_mode = staged_power_mode_;
    a_args.seamless = false;
    a_args.blocking = true;
  }

  auto hdcp_state = hdcpcon_ ? hdcpcon_->GetHdcpState()
                             : HdcpController::HdcpState::kUndesired;
  if (hdcp_state == HdcpController::HdcpState::kDesired) {
    a_args.content_protection = ContentProtection::kDesired;
    a_args.hdcp_content_type = HdcpContentType::kType1;
  }
  if (hdcp_state == HdcpController::HdcpState::kRetry) {
    a_args.content_protection = ContentProtection::kDesired;
    a_args.hdcp_content_type = HdcpContentType::kType0;
  }

  a_args.composition = composition.composition_plan
                           ? composition.composition_plan
                           : CreateLayerToPlaneJoiningPlan(
                                 composition.composition_types);
  if (!a_args.composition) {
    return std::nullopt;
  }

  const bool all_client_layers = a_args.composition->client_z_order &&
                                 a_args.composition->plan.size() == 1;
  // When client CTM will be applied by the GPU, only apply render intent CTM
  // via DRM.
  if (all_client_layers && CtmByGpu() &&
      hwc_->GetResMan().GetCtmHandling() == CtmHandling::kDrmOrGpu) {
    a_args.color_matrix = render_intent_matrix_;
  }

  // Client CTM with offset cannot be processed by CTM prop. Only apply render
  // intent CTM which will never have offset.
  if (client_ctm_has_offset_ && !(HasHardwareColorTransform() ||
                                  hwc_->GetResMan().ForceCtmHandlesOffset())) {
    a_args.color_matrix = render_intent_matrix_;
  }

  if (pipeline_->writeback_connector) {
    if (!writeback_layer_->IsLayerUsableAsDevice()) {
      ALOGE("Writeback layer not usable by DRM/KMS - no valid buffer set");
      return std::nullopt;
    }
    a_args.writeback_fb = writeback_layer_->GetLayerData().fb;
    a_args.writeback_release_fence = writeback_layer_->GetLayerData()
                                         .acquire_fence;
  }
  return a_args;
}

std::unique_ptr<LayerToPlaneJoiningPlan>
HwcDisplay::CreateLayerToPlaneJoiningPlan(
    const CompositionPlanner::CompositionTypeMap &composition_types) const {
  std::optional<uint32_t> client_z_order;
  std::map<uint32_t, const HwcLayer *> z_map;
  std::optional<LayerData> cursor_layer = std::nullopt;
  for (const auto &[_, layer] : layers_) {
    auto it = composition_types.find(&layer);
    CompositionType type = it != composition_types.end()
                               ? it->second
                               : CompositionType::kInvalid;
    switch (type) {
      case CompositionType::kDevice:
        z_map.emplace(layer.GetZOrder(), &layer);
        break;
      case CompositionType::kCursor:
        if (!cursor_layer.has_value()) {
          cursor_layer = layer.GetLayerData();
        } else {
          ALOGW("Detected multiple cursor layers");
          z_map.emplace(layer.GetZOrder(), &layer);
        }
        break;
      case CompositionType::kClient:
        // Place it at the z_order of the lowest client layer
        client_z_order = std::min(client_z_order.value_or(UINT32_MAX),
                                  layer.GetZOrder());
        break;
      case CompositionType::kDeviceOccluded:
        // Occluded layers are neither client nor device composited. Since they
        // are not visible, their absence should not have an impact on the
        // correctness of the displayed frame.
        break;
      case CompositionType::kSolidColor:
        // Skip solid color layers
        continue;
      case CompositionType::kInvalid:
        ALOGE("Invalid layer type: %d", static_cast<int>(type));
        continue;
    }
  }

  // When layers_ is empty, the loop above does not assign client_z_order. Place
  // client_layer_ at z_order 0 so pure client-target compositions are committed
  // to the primary plane.
  if (layers_.empty() && client_layer_.IsLayerUsableAsDevice()) {
    client_z_order = 0;
  }

  if (client_z_order.has_value()) {
    z_map.emplace(client_z_order.value(), &client_layer_);
    if (!client_layer_.IsLayerUsableAsDevice()) {
      /* This may be normally triggered on validation of the first frame
       * containing CLIENT layer. At this moment client buffer is not yet
       * provided by the CLIENT.
       * This may be triggered once in HwcLayer lifecycle in case FB can't be
       * imported. For example when non-contiguous buffer is imported into
       * contiguous-only DRM/KMS driver.
       */
      return nullptr;
    }
  }

  ALOGW_IF(z_map.empty() && !cursor_layer.has_value(), "Empty composition");

  std::vector<LayerData> composition_layers;

  // now that they're ordered by z, add them to the composition
  for (const auto &[_, layer] : z_map) {
    if (!layer->IsLayerUsableAsDevice()) {
      return nullptr;
    }
    composition_layers.emplace_back(layer->GetLayerData());
  }
  auto composition = LayerToPlaneJoiningPlan::
      CreateLayerToPlaneJoiningPlan(GetPipe(), std::move(composition_layers),
                                    std::move(cursor_layer));
  if (composition) {
    composition->client_z_order = client_z_order;
  }
  return composition;
}

CommitStatus HwcDisplay::CommitStagedComposition(SharedFd &out_present_fence) {
  ATRACE_CALL();

  if (IsInHeadlessMode()) {
    ALOGE("%s: Display is in headless mode, should never reach here", __func__);
    return CommitStatus::Success();
  }

  if (!validated_composition_.has_value()) {
    ALOGE("%s: No composition is staged. Cannot commit.", __func__);
    return CommitStatus::InternalFailure();
  }

  PrepareCompositionForCommit(validated_composition_.value());
  auto a_args = CreateFrameUpdateCommit(validated_composition_.value());
  if (a_args) {
    last_presented_composition_.SetValidatedComposition(
        *validated_composition_);
  }
  // |validated_composition_| can safely be reset now. |a_args| holds its own
  // pointer to the plan which will remain in scope until the commit is finished
  // (successfully or not).
  validated_composition_.reset();

  if (!a_args) {
    ALOGE("Failed to create AtomicCommitArgs for frame composition.");
    if (staged_mode_config_id_ &&
        GetConfig(*staged_mode_config_id_) == nullptr) {
      ClearStagedModeConfig();
    }
    last_presented_composition_.Reset();
    return CommitStatus::InternalFailure();
  }

  const auto status_or_result = ExecuteAtomicCommit(*a_args);
  const auto &result = status_or_result.GetValue();
  if (!result.has_value()) {
    ALOGE("Failed to commit the frame composition with err=%d",
          status_or_result.GetStatus().error_code);
    if (active_config_id_ != 0 && a_args->display_mode &&
        staged_mode_config_id_ &&
        a_args->display_config_id == *staged_mode_config_id_) {
      ClearStagedModeConfig();
      if (hwc_) {
        hwc_->SendRefreshEventToClient(handle_);
      }
    }
    last_presented_composition_.Reset();
    return status_or_result.GetStatus();
  }

  out_present_fence = result->present_fence;
  ApplyCommitChanges(*a_args, result.value());
  return CommitStatus::Success();
}

void HwcDisplay::ApplyCommitChanges(const AtomicCommitArgs &a_args,
                                    const AtomicCommitResult &result) {
  writeback_complete_fence_ = result.writeback_complete_fence;
  if (a_args.power_mode) {
    staged_power_mode_.reset();
    if (*a_args.power_mode != PowerMode::kOff) {
      defer_modeset_until_power_on_ = false;
    }
  }
  if (a_args.display_mode) {
    // Get the vsync period before updating active_config_id.
    uint32_t prev_vperiod_ns = GetCurrentVsyncPeriodNs();
    ClearStagedModeConfig();
    uint32_t last_vsync_ts = vsync_worker_->GetLastVsyncTimestamp();
    if (last_vsync_ts != 0) {
      hwc_->SendVsyncPeriodTimingChangedEventToClient(handle_,
                                                      last_vsync_ts +
                                                          prev_vperiod_ns);
    }

    // If display_config_id is nullopt that indicates a logic error.
    ALOGE_IF(!a_args.display_config_id,
             "a_args.display_mode is set but a_args.display_config_id is not.");
    // Update the active_config_id and update the vsync period for the
    // VsyncWorker.
    active_config_id_ = a_args.display_config_id.value_or(active_config_id_);
    if (!a_args.seamless) {
      link_recovery_modeset_required_ = false;
    }
    vsync_worker_->SetVsyncPeriodNs(a_args.display_mode->GetVSyncPeriodNs());
    // When a modeset commit is executed alongside a power transition (such as
    // CreateModesetCommit() rather than a seamless frame-update modeset that
    // already carries the client's validated composition), invalidate the
    // cached composition and request a refresh so SurfaceFlinger presents a
    // fresh frame for the newly active configuration.
    if (a_args.power_mode.has_value()) {
      last_presented_composition_.Reset();
      hwc_->SendRefreshEventToClient(handle_);
    }
  }

  if (a_args.hdcp_content_type.has_value() ||
      a_args.content_protection.has_value()) {
    if (a_args.hdcp_content_type.has_value()) {
      ALOGI("HDCP commit requested with Content Type %d for display %d",
            static_cast<int>(a_args.hdcp_content_type.value()),
            static_cast<int>(handle_));
    }
    if (hdcpcon_ == nullptr) {
      ALOGE("HDCP commit completed without an HDCP controller for display %d",
            static_cast<int>(handle_));
    } else {
      hdcpcon_->Requested();
    }
  }
}

size_t HwcDisplay::GetNumAvailablePlanes() const {
  return pipeline_->GetUsablePlanes().first.size();
}

std::shared_ptr<BindingOwner<DrmPlane>> HwcDisplay::GetCursorPlane() const {
  return pipeline_->GetUsablePlanes().second;
}

bool HwcDisplay::HasHardwareColorTransform() const {
  if (IsInHeadlessMode()) {
    return false;
  }
  if (pipeline_ && pipeline_->capabilities) {
    const auto ctm_override = pipeline_->capabilities
                                  ->GetHardwareColorTransformOverride();
    if (ctm_override.has_value()) {
      return *ctm_override;
    }
  }
  return UseColorPipeline() && GetPipe().crtc && GetPipe().crtc->Get() &&
         GetPipe().crtc->Get()->GetCtmProperty() &&
         GetPipe().crtc->Get()->GetCtmOffsetProperty();
}

bool HwcDisplay::CtmByGpu() const {
  if (client_color_matrix_ == GetIdentityCtmPtr())
    return false;

  if (HasHardwareColorTransform())
    return false;

  if (GetPipe().crtc->Get()->GetCtmProperty() && !client_ctm_has_offset_)
    return false;

  if (hwc_->GetResMan().GetCtmHandling() == CtmHandling::kDrmOrIgnore)
    return false;

  return true;
}

bool HwcDisplay::ForcedScalingWithGpu() const {
  return hwc_->GetResMan().ForcedScalingWithGpu();
}

bool HwcDisplay::IsWritebackSupported() const {
  return !IsInHeadlessMode() && !is_virtual_ &&
         pipeline_->FindWritebackConnectorForPipeline() != nullptr;
}

bool HwcDisplay::SetWritebackEnabled(bool enabled) {
  if (IsInHeadlessMode()) {
    return false;
  }

  // Handle Disable
  if (!enabled) {
    pipeline_->writeback_connector = nullptr;
    return true;
  }

  // Handle Enable
  if (pipeline_->writeback_connector != nullptr) {
    return true;
  }

  auto *wb_connector = pipeline_->FindWritebackConnectorForPipeline();
  if (!wb_connector) {
    ALOGE("HwcDisplay: No writeback connector found");
    return false;
  }
  auto bound_connector = wb_connector->BindPipeline(pipeline_.get());
  if (!bound_connector) {
    ALOGE("HwcDisplay: Failed to bind writeback connector");
    return false;
  }
  pipeline_->writeback_connector = bound_connector;
  return true;
}

SharedFd HwcDisplay::GetWritebackBufferFence() {
  if (!writeback_complete_fence_) {
    ALOGE("HwcDisplay: No readback fence available for display");
    return nullptr;
  }

  return std::move(writeback_complete_fence_);
}

std::vector<const HwcLayer *> HwcDisplay::GetOrderLayersByZPos() const {
  std::vector<const HwcLayer *> ordered_layers;
  ordered_layers.reserve(layers_.size());

  for (const auto &[handle, layer] : layers_) {
    ordered_layers.emplace_back(&layer);
  }

  std::sort(std::begin(ordered_layers), std::end(ordered_layers),
            [](const HwcLayer *lhs, const HwcLayer *rhs) {
              // Cursor layers should always have highest zpos.
              if ((lhs->GetSfType() == CompositionType::kCursor) !=
                  (rhs->GetSfType() == CompositionType::kCursor)) {
                return rhs->GetSfType() == CompositionType::kCursor;
              }

              return lhs->GetZOrder() < rhs->GetZOrder();
            });

  return ordered_layers;
}

// Display primary values are coded as unsigned 16-bit values in units of
// 0.00002, where 0x0000 represents zero and 0xC350 represents 1.0000.
static uint64_t ToU16ColorValue(float in) {
  constexpr float kPrimariesFixedPoint = 50000.F;
  return static_cast<uint64_t>(kPrimariesFixedPoint * in);
}

void HwcDisplay::SetHdrOutputMetadata(const android::ColorSpace &color_gamut,
                                      TransferFunction transfer_function) {
  hdr_metadata_ = std::make_shared<hdr_output_metadata>();
  hdr_metadata_->metadata_type = 0;
  auto *m = &hdr_metadata_->hdmi_metadata_type1;
  m->metadata_type = 0;

  switch (transfer_function) {
    case TransferFunction::kSmpte170M:
      m->eotf = 1;
      transfer_func_ = transfer_function;
      break;
    case TransferFunction::kPq:
      m->eotf = 2;
      transfer_func_ = transfer_function;
      break;
    case TransferFunction::kHlg:
      m->eotf = 3;
      transfer_func_ = transfer_function;
      break;
    case TransferFunction::kUnknown:
      [[fallthrough]];
    default:
      ALOGW("Transfer function %d is not supported.", transfer_function);
      transfer_func_ = TransferFunction::kUnknown;
      return;
  }

  // Most luminance values are coded as an unsigned 16-bit value in units of 1
  // cd/m2, where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2.
  std::vector<ui::Hdr> types;
  float hdr_luminance[3]{0.F, 0.F, 0.F};
  if (GetEdid()) {
    GetEdid()->GetHdrCapabilities(types, &hdr_luminance[0], &hdr_luminance[1],
                                  &hdr_luminance[2]);
  }
  m->max_display_mastering_luminance = m->max_cll = static_cast<uint64_t>(
      hdr_luminance[0]);
  m->max_fall = static_cast<uint64_t>(hdr_luminance[1]);
  // The min luminance value is coded as an unsigned 16-bit value in units of
  // 0.0001 cd/m2, where 0x0001 represents 0.0001 cd/m2 and 0xFFFF
  // represents 6.5535 cd/m2.
  m->min_display_mastering_luminance = static_cast<uint64_t>(hdr_luminance[2] *
                                                             10000.F);

  auto primaries = color_gamut.getPrimaries();
  m->display_primaries[0].x = ToU16ColorValue(primaries[0].x);
  m->display_primaries[0].y = ToU16ColorValue(primaries[0].y);
  m->display_primaries[1].x = ToU16ColorValue(primaries[1].x);
  m->display_primaries[1].y = ToU16ColorValue(primaries[1].y);
  m->display_primaries[2].x = ToU16ColorValue(primaries[2].x);
  m->display_primaries[2].y = ToU16ColorValue(primaries[2].y);

  auto whitePoint = color_gamut.getWhitePoint();
  m->white_point.x = ToU16ColorValue(whitePoint.x);
  m->white_point.y = ToU16ColorValue(whitePoint.y);
}

bool HwcDisplay::NeedsClientLayerUpdate() const {
  return std::any_of(layers_.begin(), layers_.end(), [](const auto &pair) {
    const auto &layer = pair.second;
    return layer.GetSfType() == CompositionType::kClient ||
           layer.GetValidatedType() == CompositionType::kClient;
  });
}

auto HwcDisplay::GetWritebackBufferFormat() const -> BufferFormat {
  const bool has_hdr_layer = std::any_of(layers_.begin(), layers_.end(),
                                         [](const auto &pair) {
                                           const auto tf = pair.second
                                                               .GetLayerData()
                                                               .transfer_func;
                                           return tf == TransferFunction::kPq ||
                                                  tf == TransferFunction::kHlg;
                                         });
  return (has_hdr_support_ && has_hdr_layer) ? BufferFormat::kRgbaFp16
                                             : BufferFormat::kRgba8888;
}

bool HwcDisplay::CanReuseLayerForModeset(uint32_t width,
                                         uint32_t height) const {
  if (link_recovery_modeset_required_) {
    return false;
  }

  const HwcDisplayConfig *active_config = GetCurrentConfig();
  // Reuse an existing layer only when the CRTC is already active at the
  // target resolution. After a teardown (power-off), cached buffers may
  // contain stale content that we do not want to rescan on modeset.
  return active_config != nullptr &&
         GetPipe().atomic_state_manager->IsActive() &&
         active_config->mode.GetRawMode().hdisplay == width &&
         active_config->mode.GetRawMode().vdisplay == height;
}

std::optional<LayerData> HwcDisplay::FindFullScreenDeviceLayerForModeset(
    uint32_t width, uint32_t height) const {
  for (const HwcLayer *layer : GetOrderLayersByZPos()) {
    if (!IsUnscaledFullScreenDeviceLayer(*layer, width, height)) {
      continue;
    }
    LayerData modeset_layer_data = layer->GetLayerData();
    // Clear acquire_fence because the layer buffer was already acquired and
    // presented on a previous frame.
    modeset_layer_data.acquire_fence = {};
    return modeset_layer_data;
  }
  return std::nullopt;
}

std::optional<LayerData> HwcDisplay::GetExistingModesetLayerData(
    uint32_t width, uint32_t height) const {
  if (!CanReuseLayerForModeset(width, height)) {
    return std::nullopt;
  }

  // Prefer client_layer_ immediately only when client composition is active
  // on the current frame. During pure DEVICE composition
  // (!NeedsClientLayerUpdate()), client_layer_ is either unpopulated or holds
  // a stale client target buffer from an earlier CLIENT-composited frame.
  const bool is_client_composition_active = NeedsClientLayerUpdate() ||
                                            layers_.empty();
  if (is_client_composition_active && client_layer_.IsLayerUsableAsDevice()) {
    ALOGV("Use existing client_layer for config.");
    return client_layer_.GetLayerData();
  }

  // Search active layers for an unscaled full-screen DEVICE layer matching
  // the target mode dimensions so same-resolution modesets (such as
  // refresh-rate switches on external displays) continue scanning out the
  // active buffer rather than flashing a stale client target or a zero-filled
  // dumb buffer.
  if (auto device_layer = FindFullScreenDeviceLayerForModeset(width, height)) {
    ALOGV("Use existing device layer for config.");
    return device_layer;
  }

  if (client_layer_.IsLayerUsableAsDevice()) {
    ALOGV("Use existing client_layer for config.");
    return client_layer_.GetLayerData();
  }

  return std::nullopt;
}

std::optional<LayerData> HwcDisplay::GetModesetLayerData(
    const HwcDisplayConfig *new_config) {
  if (IsInHeadlessMode()) {
    return std::nullopt;
  }

  const uint32_t new_width = new_config->mode.GetRawMode().hdisplay;
  const uint32_t new_height = new_config->mode.GetRawMode().vdisplay;

  if (auto existing_layer = GetExistingModesetLayerData(new_width,
                                                        new_height)) {
    return existing_layer;
  }

  ALOGV("Allocate modeset buffer.");
  std::optional<BufferInfo>
      modeset_buffer = GetPipe().device->CreateDumbBuffer(new_width, new_height,
                                                          DRM_FORMAT_XRGB8888);
  if (!modeset_buffer)
    return std::nullopt;

  auto fb = GetPipe().importer->GetOrCreateFbId(&modeset_buffer.value());
  // Return nullopt if framebuffer import fails so we do not construct a
  // LayerData with a null fb (which triggers an abort inside
  // DrmAtomicStateManager).
  if (!fb) {
    ALOGE("Failed to import modeset buffer.");
    return std::nullopt;
  }

  auto modeset_layer = std::make_unique<HwcLayer>(this);
  modeset_layer->SetLayerProperties({
      .buffer = std::optional<HwcLayer::Buffer>({
          .bi = modeset_buffer.value(),
          .fb = std::move(fb),
          .fence = {},
      }),
      .blend_mode = BufferBlendMode::kNone,
  });

  return modeset_layer->GetLayerData();
}

void HwcDisplay::SetConfigGroupsForActiveConfig() {
  const auto *active_config = GetCurrentConfig();
  if (!active_config) {
    ALOGW("Could not fetch active config for config group assignment.");
    return;
  }

  const std::optional<LayerData> modeset_layer_data = GetModesetLayerData(
      active_config);
  for (auto &[_, config] : configs_.hwc_configs) {
    if (config.output_type != active_config->output_type) {
      continue;
    }

    AtomicCommitArgs commit_args = CreateModesetCommit(&config,
                                                       modeset_layer_data);
    commit_args.seamless = true;
    if (GetPipe()
            .device->GetAtomicCommitSink()
            .TestAtomicCommit(
                {{GetPipe().atomic_state_manager.get(), commit_args}})
            .success) {
      config.group_id = active_config->group_id;
    }
  }

  configs_.SanitizeGroups();
}

std::pair<uint32_t, uint32_t> HwcDisplay::GetSize() const {
  const auto *config = GetNextConfig();
  if (config == nullptr) {
    return std::make_pair(0, 0);
  }
  return std::make_pair(config->mode.GetRawMode().hdisplay,
                        config->mode.GetRawMode().vdisplay);
}

auto HwcDisplay::SetBrightness(float brightness) -> bool {
  if (std::isnan(brightness) || brightness > kMaxBrightness ||
      (brightness < kMinBrightness && !FloatEquals(brightness, -1.0F))) {
    return false;
  }

  if (brightness >= kMinBrightness &&
      GetPipe().connector->Get()->IsInternal()) {
    const float scaled_brightness = ColorUtil::ScaleBrightnessIfNeeded(
        brightness);
    if (HasBacklight() &&
        !backlight_controller_->SetBrightness(scaled_brightness)) {
      return false;
    }
    brightness_ = scaled_brightness;
    return true;
  }

  // -1.0F indicates turning the backlight off per HAL API.
  // Set to 0 so downstream color and gamma pipelines scale to 0 (black).
  if (HasBacklight() && !backlight_controller_->SetBrightness(std::nullopt)) {
    return false;
  }
  brightness_ = 0.0F;
  return true;
}

void HwcDisplay::SetBrightnessNits(float brightness_nits) {
  if (IsInHeadlessMode() || pipeline_->connector->Get()->IsExternal()) {
    return;
  }

  // Negative values indicate an unknown value per HAL API.
  if (brightness_nits < 0.F) {
    brightness_nits_ = std::nullopt;
  } else {
    brightness_nits_ = brightness_nits;
  }
}

void HwcDisplay::LogModesOnHotplug() {
  if (!display_mode_reporter_) {
    return;
  }

  const HwcDisplay::DisplayType display_type = GetDisplayType();
  if (display_type != HwcDisplay::DisplayType::kInternal &&
      display_type != HwcDisplay::DisplayType::kExternal) {
    return;
  }

  const uint32_t
      connection_type = GetPipe().connector->Get()->GetConnectorType();
  const bool is_mst = GetPipe().connector->Get()->IsMst();

  auto vendor = EdidWrapper::VendorProductInfo{};
  uint32_t vrr_range_min = 0;
  uint32_t vrr_range_max = 0;
  std::vector<ui::Hdr> hdr_types;
  float max_luminance = 0.0F;
  float max_average_luminance = 0.0F;
  float min_luminance = 0.0F;
  if (edid_wrapper_ != nullptr) {
    vendor = edid_wrapper_->GetVendorProductInfo();
    std::tie(vrr_range_min,
             vrr_range_max) = edid_wrapper_->GetVerticalDisplayRangeLimits();
    edid_wrapper_->GetHdrCapabilities(hdr_types, &max_luminance,
                                      &max_average_luminance, &min_luminance);
  }

  using ModeAtom = DisplayHotplugConnectModeDetectedAtomReporter::Atom;
  std::vector<ModeAtom> submitted_atoms;
  for (const auto &[id, hwc_mode] : configs_.hwc_configs) {
    const DrmMode &mode = hwc_mode.mode;
    const drmModeModeInfo &raw_mode = mode.GetRawMode();
    const bool is_preferred = (raw_mode.type & DRM_MODE_TYPE_PREFERRED) != 0;

    constexpr float kMmPerInch = 25.4;
    const auto [width_mm, height_mm] = GetDisplayBoundsMm();
    int32_t dpi_x = -1;
    if (width_mm > 0) {
      dpi_x = static_cast<int32_t>(
          lround((static_cast<float>(raw_mode.hdisplay) * kMmPerInch) /
                 static_cast<float>(width_mm)));
    }
    int32_t dpi_y = dpi_x;
    if (height_mm > 0) {
      dpi_y = static_cast<int32_t>(
          lround((static_cast<float>(raw_mode.vdisplay) * kMmPerInch) /
                 static_cast<float>(height_mm)));
    }

    using AtomDisplayType = DisplayHotplugConnectModeDetectedAtomReporter::
        DisplayType;
    const ModeAtom atom =
        {.display_handle = handle_,
         .resolution_x = raw_mode.hdisplay,
         .resolution_y = raw_mode.vdisplay,
         .refresh_rate = static_cast<int32_t>(lround(mode.GetVRefresh())),
         .dpi_x = dpi_x,
         .dpi_y = dpi_y,
         .display_type = display_type == HwcDisplay::DisplayType::kInternal
                             ? AtomDisplayType::kInternal
                             : AtomDisplayType::kExternal,
         .is_preferred = is_preferred,
         .make = vendor.make,
         .model = vendor.model,
         .year = vendor.year,
         .hdr_types = hdr_types,
         .max_luminance = max_luminance,
         .max_average_luminance = max_average_luminance,
         .min_luminance = min_luminance,
         .connection_type = connection_type,
         .has_path = is_mst,
         .vrr_range_min = vrr_range_min,
         .vrr_range_max = vrr_range_max};

    if (std::find(submitted_atoms.begin(), submitted_atoms.end(), atom) !=
        submitted_atoms.end()) {
      continue;
    }

    display_mode_reporter_->PushAtom(atom);
    submitted_atoms.push_back(atom);
  }
}

void HwcDisplay::LogConfigResult(const AtomicCommitArgs &args, bool is_success,
                                 int64_t duration_ns) const {
  if (!config_result_reporter_) {
    return;
  }

  DisplayConfigurationResultReporter::DisplayType
      display_type = DisplayConfigurationResultReporter::DisplayType::
          kUnspecified;
  switch (GetDisplayType()) {
    case HwcDisplay::DisplayType::kInternal:
      display_type = DisplayConfigurationResultReporter::DisplayType::kInternal;
      break;
    case HwcDisplay::DisplayType::kExternal:
      display_type = DisplayConfigurationResultReporter::DisplayType::kExternal;
      break;
    default:
      display_type = DisplayConfigurationResultReporter::DisplayType::
          kUnspecified;
      break;
  }

  const DisplayConfigurationResultReporter::Atom atom{
      .display_handle = handle_,
      .success = is_success,
      .is_seamless = args.seamless,
      .display_type = display_type,
      .is_blocking = args.blocking,
      .is_display_mode = args.display_mode.has_value(),
      .is_power_mode = args.power_mode.has_value(),
      .is_teardown = args.teardown,
      .duration_ns = duration_ns,
  };
  config_result_reporter_->PushAtom(atom);
}

bool HwcDisplay::CursorPlaneNeedsColorPipeline(
    const HwcLayer &cursor_layer) const {
  if (!UseColorPipeline()) {
    return false;
  }

  const HwcColorspace cursor_colorspace = cursor_layer.GetLayerData()
                                              .colorspace;
  auto cursor_matrix = ColorUtil::GamutAdjustIfNeeded<
      drm_color_ctm_3x4>(cursor_colorspace, colorspace_, color_matrix_);

  if (!cursor_matrix) {
    return false;
  }

  static const auto identity_3x4 = ColorUtil::ToColorTransform3x4(
      GetIdentityCtmPtr());

  return (memcmp(cursor_matrix->matrix, identity_3x4->matrix,
                 sizeof(identity_3x4->matrix)) != 0);
}

std::string HwcDisplay::Dump() const {
  std::stringstream ss;
  if (pipeline_ && pipeline_->connector && pipeline_->connector->Get()) {
    if (auto tile_info = pipeline_->connector->Get()->GetTileInfo()) {
      ss << "Tile Info: group_id=" << tile_info->group_id
         << " single_monitor=" << tile_info->tile_is_single_monitor
         << " grid=" << tile_info->num_h_tile << "x" << tile_info->num_v_tile
         << " loc=(" << tile_info->tile_h_loc << "," << tile_info->tile_v_loc
         << ") tile_size=" << tile_info->tile_h_size << "x"
         << tile_info->tile_v_size << "\n";
    }
  }
  if (pipeline_ && pipeline_->atomic_state_manager) {
    ss << pipeline_->atomic_state_manager->DumpState();
  }
  return ss.str();
}

}  // namespace android::drm_hwcomposer
