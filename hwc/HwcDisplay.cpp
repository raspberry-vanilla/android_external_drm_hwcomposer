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
#include <linux/time.h>
#include <ui/ColorSpace.h>
#include <ui/GraphicTypes.h>
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

namespace android::drm_hwcomposer {

using FlattenReason = CompositionPlanner::FlattenReason;

namespace {

// NOLINTBEGIN(misc-include-cleaner)
constexpr auto kFlatteningTimeout = 1s;
constexpr auto kHdcpRetryTimeout = 30s;
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
        ColorUtil::ToLinearCtm(color_transform_matrix, color_mode_));
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

void HwcDisplay::SetHdrHeadroom() {
  float hdr_luminance[3]{kDefaultMaxLuminance, kDefaultMaxLuminance, 0.F};
  GetEdid()->GetHdrLuminance(&hdr_luminance[0], &hdr_luminance[1],
                             &hdr_luminance[2]);
  float max_lum = hdr_luminance[0] > 0.F ? hdr_luminance[0]
                                         : kDefaultMaxLuminance;
  hdr_headroom_ = max_lum / kHdrReferenceLuminance;
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
      GetEdid()->GetSupportedHdrTypes(hdr_types);
      if (hdr_types.empty() && !forced_color_mode_) {
        SetHdrOutputMetadata(kBt2020Gamut, TransferFunction::kSrgb);
        min_bpc_ = 6;
        colorspace_ = HwcColorspace::kBt2020;
        break;
      }

      // TODO(b/553171429): Clean up sentinel once early boot animation is active.
      if (boot_animation_state_ == BootAnimationState::kActive) {
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
          SetHdrOutputMetadata(kBt2020Gamut,
                               TransferFunction::kSmpte170M);
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
  if (IsInHeadlessMode()) {
    active_config_id_ = config;
    hwc_->LogRefreshRateChanges();
    return ConfigError::kNone;
  }

  ALOGV("Create modeset commit.");
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
  staged_mode_config_id_.reset();
  // set new vsync period
  vsync_worker_->SetVsyncPeriodNs(new_config->mode.GetVSyncPeriodNs());
  hwc_->LogRefreshRateChanges();
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
  if (!current_config || current_config->group_id != new_config->group_id) {
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

  if (layers_.empty()) {
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

  if (boot_animation_state_ == BootAnimationState::kCleanup) {
    boot_animation_state_ = BootAnimationState::kInactive;
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

  const auto bounds = GetEdid()->GetBoundsMm();
  if (bounds.first > 0 || bounds.second > 0) {
    return bounds;
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

  if (layers_.empty()) {
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

  /*
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
  */
  return handle_; /* TODO: What should be here? */
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
    return HwcDisplay::Error::kNone;
  }
  bool enabled = mode != PowerMode::kOff;

  // If the request is to enable the display, the CRTC is not active, and an
  // active config is set, try to reconfigure the pipeline with SetConfig.
  if (enabled) {
    if (GetPipe().atomic_state_manager->IsActive()) {
      return HwcDisplay::Error::kNone;
    }

    const HwcDisplayConfig *last_requested_config = GetLastRequestedConfig();
    if (last_requested_config) {
      if (SetConfig(last_requested_config->id) != ConfigError::kNone) {
        ALOGE("Failed to set config to re-enable display after teardown.");
        return HwcDisplay::Error::kBadParameter;
      }
    }
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
  // If setting to |enabled|, log the error and return true. The next frame
  // update will try to set it to active again.
  if (!result.IsSuccess() && !enabled) {
    return HwcDisplay::Error::kBadParameter;
  }
  return HwcDisplay::Error::kNone;
}

bool HwcDisplay::GetDisplayEnabled() const {
  if (IsInHeadlessMode()) {
    return true;
  }

  return GetPipe().atomic_state_manager->IsActive();
}

void HwcDisplay::SetPipeline(std::shared_ptr<DrmDisplayPipeline> pipeline) {
  Deinit();

  pipeline_ = std::move(pipeline);

  if (pipeline_ != nullptr || handle_ == kPrimaryDisplay) {
    bool success = Init();
    ALOGE_IF(!success, "Failed to init HwcDisplay after setting pipeline.");
    hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kConnected);
    if (pipeline_) {
      LogModesOnHotplug();
    }
  } else {
    hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kDisconnected);
  }
}

void HwcDisplay::Deinit() {
  if (pipeline_ != nullptr) {
    AtomicCommitArgs a_args{};
    a_args.composition = std::make_shared<LayerToPlaneJoiningPlan>();
    ExecuteAtomicCommit(a_args);
    a_args.composition = {};
    a_args.power_mode = PowerMode::kOff;
    a_args.teardown = true;
    ExecuteAtomicCommit(a_args);

    validated_composition_.reset();
    flatcon_.reset();
    hdcpcon_.reset();
    backlight_controller_.reset();
    last_presented_composition_.Reset();
  }

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
  GetEdid()->GetColorModes(color_modes);
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
  GetEdid()->GetSupportedHdrTypes(hdr_types);

  if (!GetPipe().connector || !GetPipe().connector->Get()) {
    has_hdr_support_ = false;
    return;
  }

  const bool hdr_sysprop_enabled = GetPipe().connector->Get()->IsExternal()
                                       ? hwc_->GetResMan().ExternalHdrEnabled()
                                       : hwc_->GetResMan()
                                             .PersistentHdrEnabled();

  // TODO check for HDR types
  has_hdr_support_ = use_color_pipeline_ && crtc_gamma && has_wcg_support_ &&
                     hdr_sysprop_enabled &&
                     GetPipe()
                         .connector->Get()
                         ->GetHdrOutputMetadataProperty() &&
                     GetPipe().connector->Get()->GetColorspaceProperty() &&
                     (!hdr_types.empty() ||
                      (forced_color_mode_ &&
                       forced_color_mode_.value() != ColorMode::kNative));
}

bool HwcDisplay::Init() {
  boot_animation_state_ = BootAnimationState::kActive;

  if (!is_virtual_) {
    vsync_worker_ = VSyncWorker::CreateInstance(pipeline_);
    if (!vsync_worker_) {
      ALOGE("Failed to create event worker for d=%d\n", int(handle_));
      return false;
    }
  }

  if (!IsInHeadlessMode()) {
    if (Properties::FlatteningEnabled()) {
      auto flatcbk = (struct FlatConCallbacks){
          .trigger = [this]() { hwc_->SendRefreshEventToClient(handle_); }};
      flatcon_ = std::make_unique<FlatteningController>(handle_, flatcbk,
                                                        kFlatteningTimeout);
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
    auto edid = LibdisplayEdidWrapper::Create(
        pipeline_->connector->Get()->GetEdidBlob());
    if (edid) {
      edid_wrapper_ = std::move(edid);
    } else {
      ALOGW("Failed to create a LibdisplayInfo parser.");
    }
#endif

    // Attempt to initialize backlight
    if (GetPipe().connector->Get()->IsInternal()) {
      auto backlights = SysfsBacklightController::EnumerateBacklights();
      for (const auto &name : backlights) {
        // TODO(seanpaul): logic to associate backlight with connector
        backlight_controller_ = SysfsBacklightController::CreateInstanceFromName(
            name);
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

    auto *connector = pipeline_->connector->Get();
    auto ret = connector->UpdateModesAndProperties();
    if (ret != 0) {
      ALOGE("Failed to update display modes with error: %d", ret);
      return false;
    }

    const HwcConfigParameters params = {
        .use_color_pipeline = Properties::UseColorPipeline(),
        .persistent_hdr_enabled = Properties::PersistentHdrEnabled(),
        .external_hdr_enabled = Properties::ExternalHdrEnabled(),
        .capabilities = pipeline_->capabilities.get(),
        .min_refresh_rate = Properties::MinRefreshRate(),
        .max_refresh_rate = Properties::MaxRefreshRate(),
        .force_disable_mrr = Properties::ForceDisableMrr(),
    };
    auto configs = configs_generator_.GenerateDisplayConfigs(*connector,
                                                             params);
    if (!configs) {
      return false;
    }
    configs_ = std::move(*configs);
  }

  InitForcedColorMode();
  InitUseColorPipeline();
  InitWcgSupported();
  InitHdrSupported();

  static constexpr ColorMode kDefaultColorMode = ColorMode::kSrgb;
  SetColorMode(kDefaultColorMode, ui::RenderIntent::COLORIMETRIC);

  if (SetConfig(configs_.preferred_config_id) !=
      HwcDisplay::ConfigError::kNone) {
    return false;
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
  if (boot_animation_state_ == BootAnimationState::kActive) {
    boot_animation_state_ = BootAnimationState::kCleanup;
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
  GetEdid()->GetColorModes(modes);

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

  GetEdid()->GetHdrCapabilities(*types, max_luminance, max_average_luminance,
                                min_luminance);
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
  /*
   * Hdcp will be enabled on hotplug or client can request to start Hdcp
   * With requests to start Hdcp, internal state is set to kDesired
   * else the state stays as Undesired
   * Since the HDCP Content and Content Protection prop are optional
   * We need to make sure the connector has these properties else
   * return a false to indicate that the request to start/stop
   * HDCP cannot be completed.
   */
  if (hdcpcon_ == nullptr) {
    ALOGE(
        "HDCP requested, but HDCP properties not available on that "
        "display");
    return false;
  }
  ALOGI("HDCP requested to start");
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

AtomicCommitArgs HwcDisplay::CreateModesetCommit(
    const HwcDisplayConfig *config,
    const std::optional<LayerData> &modeset_layer) {
  AtomicCommitArgs args{};

  args.brightness = brightness_;
  args.color_matrix = color_matrix_;
  args.content_type = content_type_;
  args.colorspace = colorspace_;
  args.transfer_func = transfer_func_;
  args.hdr_metadata = hdr_metadata_;
  args.min_bpc = min_bpc_;
  args.hdr_headroom = hdr_headroom_;

  std::vector<LayerData> composition_layers;
  if (modeset_layer) {
    composition_layers.emplace_back(modeset_layer.value());
  }

  if (composition_layers.empty()) {
    ALOGW("Attempting to create a modeset commit without a layer.");
  }

  args.display_mode = config->mode;
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
  a_args.color_matrix = color_matrix_;
  a_args.content_type = content_type_;
  a_args.colorspace = colorspace_;
  a_args.transfer_func = transfer_func_;
  a_args.hdr_metadata = hdr_metadata_;
  a_args.min_bpc = min_bpc_;
  a_args.hdr_headroom = hdr_headroom_;

  if (staged_mode_config_id_ &&
      staged_mode_change_time_ <= ResourceManager::GetTimeMonotonicNs()) {
    const auto *staged_config = GetConfig(staged_mode_config_id_.value());
    if (staged_config == nullptr) {
      return std::nullopt;
    }

    a_args.display_mode = staged_config->mode;
    a_args.seamless = true;
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
  if (client_ctm_has_offset_ && !HasHardwareColorTransform()) {
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
    last_presented_composition_.Reset();
    return CommitStatus::InternalFailure();
  }

  const auto status_or_result = ExecuteAtomicCommit(*a_args);
  const auto &result = status_or_result.GetValue();
  if (!result.has_value()) {
    ALOGE("Failed to commit the frame composition with err=%d",
          status_or_result.GetStatus().error_code);
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
  if (a_args.display_mode) {
    // Get the vsync period before updating active_config_id.
    uint32_t prev_vperiod_ns = GetCurrentVsyncPeriodNs();
    vsync_worker_->SetVsyncTimestampTracking(false);
    uint32_t last_vsync_ts = vsync_worker_->GetLastVsyncTimestamp();
    if (last_vsync_ts != 0) {
      hwc_->SendVsyncPeriodTimingChangedEventToClient(handle_,
                                                      last_vsync_ts +
                                                          prev_vperiod_ns);
    }

    // If staged_mode_config_id_ is nullopt that indicates a logic error.
    ALOGE_IF(!staged_mode_config_id_,
             "a_args.display_mode is set but staged_mode_config_id_ is not.");
    // Update the active_config_id and update the vsync period for the
    // VsyncWorker.
    active_config_id_ = staged_mode_config_id_.value_or(active_config_id_);
    staged_mode_config_id_.reset();
    vsync_worker_->SetVsyncPeriodNs(a_args.display_mode->GetVSyncPeriodNs());
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
  GetEdid()->GetHdrCapabilities(types, &hdr_luminance[0], &hdr_luminance[1],
                                &hdr_luminance[2]);
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

std::optional<LayerData> HwcDisplay::GetModesetLayerData(
    const HwcDisplayConfig *new_config) {
  const uint32_t new_width = new_config->mode.GetRawMode().hdisplay;
  const uint32_t new_height = new_config->mode.GetRawMode().vdisplay;

  const HwcDisplayConfig *active_config = GetCurrentConfig();
  if (client_layer_.IsLayerUsableAsDevice() && active_config &&
      // Reuse the client layer only when the CRTC is already active. After a
      // teardown (power-off), the cached buffer may contain stale content that
      // we do not want to rescan on modeset.
      GetPipe().atomic_state_manager->IsActive() &&
      active_config->mode.GetRawMode().hdisplay == new_width &&
      active_config->mode.GetRawMode().vdisplay == new_height) {
    ALOGV("Use existing client_layer for config.");
    return client_layer_.GetLayerData();
  }

  ALOGV("Allocate modeset buffer.");
  std::optional<BufferInfo>
      modeset_buffer = GetPipe().device->CreateDumbBuffer(new_width, new_height,
                                                          DRM_FORMAT_XRGB8888);
  if (!modeset_buffer)
    return std::nullopt;

  auto modeset_layer = std::make_unique<HwcLayer>(this);
  modeset_layer->SetLayerProperties({
      .buffer = std::optional<HwcLayer::Buffer>({
          .bi = modeset_buffer.value(),
          .fb = GetPipe().importer->GetOrCreateFbId(&modeset_buffer.value()),
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
  if (brightness >= 0.0F && GetPipe().connector->Get()->IsInternal()) {
    brightness_ = ColorUtil::ScaleBrightnessIfNeeded(brightness);
    if (HasBacklight()) {
      return backlight_controller_->SetBrightness(
          std::optional<float>(brightness_));
    }
    return true;
  }

  // Negative values indicate turning the backlight off per HAL API.
  // Set to 0 so downstream color and gamma pipelines scale to 0 (black).
  brightness_ = 0.0F;
  if (HasBacklight()) {
    return backlight_controller_->SetBrightness(std::nullopt);
  }
  return true;
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
  if (pipeline_ && pipeline_->atomic_state_manager) {
    return pipeline_->atomic_state_manager->DumpState();
  }
  return {};
}

}  // namespace android::drm_hwcomposer
