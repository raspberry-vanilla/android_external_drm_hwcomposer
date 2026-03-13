/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "drm/DrmMode.h"
#include "drm/drm_mode.h"
#include "hwc/HwcDisplay.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

class IDrmFbIdHandle;
struct LayerToPlaneJoiningPlan;

enum class Colorspace;
enum class ContentProtection;
enum class ContentType;
enum class HdcpContentType;
enum class PanelOrientation;

struct AtomicCommitArgs {
  /* inputs. All fields are optional, but at least one has to be specified */
  bool blocking = false;
  bool teardown = false;
  bool seamless = false;
  std::optional<DrmMode> display_mode;
  std::optional<HwcDisplay::PowerMode> power_mode;
  std::shared_ptr<LayerToPlaneJoiningPlan> composition;
  std::shared_ptr<HalColorTransforMatrix> color_matrix;
  std::optional<Colorspace> colorspace;
  std::optional<ContentType> content_type;
  std::shared_ptr<hdr_output_metadata> hdr_metadata;
  std::optional<HdcpContentType> hdcp_content_type;
  std::optional<ContentProtection> content_protection;
  std::optional<int32_t> min_bpc;

  std::shared_ptr<IDrmFbIdHandle> writeback_fb;
  SharedFd writeback_release_fence;

  /* helpers */
  auto HasInputs() const -> bool {
    return display_mode || power_mode || composition;
  }
};

struct AtomicCommitResult {
  SharedFd writeback_complete_fence;
  SharedFd present_fence;
};

class DrmAtomicCommitSink {
 public:
  virtual ~DrmAtomicCommitSink() = default;

  virtual bool TestAtomicCommit(AtomicCommitArgs &args) = 0;
  virtual std::optional<AtomicCommitResult> ExecuteAtomicCommit(
      AtomicCommitArgs &args) = 0;
  virtual bool IsActive() const = 0;
  virtual void WaitLastFrame() = 0;
};

}  // namespace android::drm_hwcomposer
