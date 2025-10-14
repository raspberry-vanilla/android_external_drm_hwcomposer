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

#pragma once

#include <memory>
#include <vector>

namespace android::drm_hwcomposer {

class DrmConnector;
class DrmDevice;
class DrmPlane;
class DrmCrtc;
class DrmEncoder;
class DrmAtomicStateManager;

struct DrmDisplayPipeline;

template <class O>
class BindingOwner;

template <class O>
class PipelineBindable {
  friend class BindingOwner<O>;

 public:
  auto *GetPipeline() {
    return bound_pipeline_;
  }

  // Header implementation required for template instantiation.
  auto BindPipeline(const DrmDisplayPipeline *pipeline,
                    bool return_object_if_bound = false)
      -> std::shared_ptr<BindingOwner<O>> {
    auto owner_object = owner_object_.lock();
    if (owner_object) {
      if (bound_pipeline_ == pipeline && return_object_if_bound) {
        return owner_object;
      }

      return {};
    }
    owner_object = std::make_shared<BindingOwner<O>>(static_cast<O *>(this));

    owner_object_ = owner_object;
    bound_pipeline_ = pipeline;
    return owner_object;
  }

 private:
  const DrmDisplayPipeline *bound_pipeline_;
  std::weak_ptr<BindingOwner<O>> owner_object_;
};

template <class B>
class BindingOwner {
 public:
  explicit BindingOwner(B *pb) : bindable_(pb){};
  ~BindingOwner() {
    bindable_->bound_pipeline_ = nullptr;
  }

  B *Get() {
    return bindable_;
  }

 private:
  B *const bindable_;
};

using UsablePlanes = std::pair<
    std::vector<std::shared_ptr<BindingOwner<DrmPlane>>>,
    std::shared_ptr<BindingOwner<DrmPlane>>>;

struct DrmDisplayPipeline {
  static auto CreatePipeline(DrmConnector &connector)
      -> std::unique_ptr<DrmDisplayPipeline>;

  auto GetUsablePlanes() const -> UsablePlanes;

  DrmConnector *FindWritebackConnectorForPipeline() const;

  ~DrmDisplayPipeline();

  DrmDevice *device;

  std::shared_ptr<BindingOwner<DrmConnector>> connector;
  std::shared_ptr<BindingOwner<DrmConnector>> writeback_connector; 
  std::shared_ptr<BindingOwner<DrmEncoder>> encoder;
  std::shared_ptr<BindingOwner<DrmCrtc>> crtc;
  std::shared_ptr<BindingOwner<DrmPlane>> primary_plane;

  std::shared_ptr<DrmAtomicStateManager> atomic_state_manager;
};

}  // namespace android::drm_hwcomposer
