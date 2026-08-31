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

#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidl/android/hardware/graphics/composer3/ChangedCompositionLayer.h>
#include <aidl/android/hardware/graphics/composer3/ClientTargetPropertyWithBrightness.h>
#include <aidl/android/hardware/graphics/composer3/CommandError.h>
#include <aidl/android/hardware/graphics/composer3/CommandResultPayload.h>
#include <aidl/android/hardware/graphics/composer3/Composition.h>
#include <aidl/android/hardware/graphics/composer3/DimmingStage.h>
#include <aidl/android/hardware/graphics/composer3/DisplayLuts.h>
#include <aidl/android/hardware/graphics/composer3/PresentFence.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#include <aidl/android/hardware/graphics/composer3/ReleaseFences.h>
#include <android-base/unique_fd.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aidl::android::hardware::graphics::composer3 {

namespace hwc3 {
enum class Error;
}  // namespace hwc3

struct DisplayChanges {
  std::optional<ChangedCompositionTypes> composition_changes;
  std::optional<DisplayRequest> display_request_changes;

  void AddLayerCompositionChange(int64_t display_handle, int64_t layer_id,
                                 Composition layer_composition) {
    if (!composition_changes) {
      composition_changes.emplace();
      composition_changes->display = display_handle;
    }

    ChangedCompositionLayer composition_change;
    composition_change.layer = layer_id;
    composition_change.composition = layer_composition;
    composition_changes->layers.emplace_back(composition_change);
  }

  void AddLayerClearRequest(int64_t display_handle, int64_t layer_id) {
    if (!display_request_changes) {
      display_request_changes.emplace();
      display_request_changes->display = display_handle;
    }
    DisplayRequest::LayerRequest layer_request;
    layer_request.layer = layer_id;
    layer_request.mask = DisplayRequest::LayerRequest::CLEAR_CLIENT_TARGET;
    display_request_changes->layerRequests.emplace_back(layer_request);
  }

  void ClearLayerCompositionChanges() {
    composition_changes.reset();
  }

  bool HasAnyChanges() const {
    return composition_changes.has_value() ||
           display_request_changes.has_value();
  }

  void Reset() {
    composition_changes.reset();
    display_request_changes.reset();
  }
};

class CommandResultWriter {
 public:
  explicit CommandResultWriter(std::vector<CommandResultPayload>* results)
      : results_(results) {
  }

  bool HasError() const {
    return has_error_;
  }

  void IncrementCommand() {
    index_++;
    has_error_ = false;
  }

  void AddError(hwc3::Error error) {
    CommandError command_error;
    command_error.errorCode = static_cast<int32_t>(error);
    command_error.commandIndex = static_cast<int32_t>(index_);

    results_->emplace_back(command_error);
    has_error_ = true;
  }

  void AddPresentFence(int64_t display_handle,
                       ::android::base::unique_fd fence) {
    if (!fence.ok()) {
      return;
    }

    PresentFence present_fence;
    present_fence.fence = ::ndk::ScopedFileDescriptor(fence.release());
    present_fence.display = display_handle;
    results_->emplace_back(std::move(present_fence));
  }

  void AddReleaseFence(
      int64_t display_handle,
      std::unordered_map<int64_t, ::android::base::unique_fd>& layer_fences) {
    ReleaseFences release_fences;
    release_fences.display = display_handle;
    for (auto& [layer, fence] : layer_fences) {
      if (!fence.ok()) {
        continue;
      }

      ReleaseFences::Layer layer_result;
      layer_result.layer = layer;
      layer_result.fence = ::ndk::ScopedFileDescriptor(fence.release());

      release_fences.layers.emplace_back(std::move(layer_result));
    }

    results_->emplace_back(std::move(release_fences));
  }

  void AddChanges(const DisplayChanges& changes) {
    if (changes.composition_changes) {
      results_->emplace_back(*changes.composition_changes);
    }
    if (changes.display_request_changes) {
      results_->emplace_back(*changes.display_request_changes);
    }
  }

  void AddPresentOrValidateResult(int64_t display_handle,
                                  const PresentOrValidate::Result& pov_result) {
    PresentOrValidate pov_command;
    pov_command.display = display_handle;
    pov_command.result = pov_result;

    results_->emplace_back(pov_command);
  }

  void AddClientTarget(int64_t display_handle,
                       const common::Dataspace dataspace,
                       const common::PixelFormat pixel_format) {
    constexpr float kDefaultBrightness = 1.F;
    ClientTargetPropertyWithBrightness client_target;
    client_target.display = display_handle;
    client_target.brightness = kDefaultBrightness;
    client_target.dimmingStage = DimmingStage::LINEAR;
    client_target.clientTargetProperty.dataspace = dataspace;
    client_target.clientTargetProperty.pixelFormat = pixel_format;
    results_->emplace_back(client_target);
  }

  void AddDisplayLuts(int64_t display_handle,
                      std::vector<DisplayLuts::LayerLut> layer_luts) {
    DisplayLuts display_luts;
    display_luts.display = display_handle;
    display_luts.layerLuts = std::move(layer_luts);
    results_->emplace_back(std::move(display_luts));
  }

 private:
  size_t index_{0};
  bool has_error_{false};
  std::vector<CommandResultPayload>* results_{nullptr};
};
};  // namespace aidl::android::hardware::graphics::composer3