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

#include <asm-generic/int-ll64.h>
#include <drm/drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "compositor/LayerData.h"
#include "drm/DrmColorOp.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"

namespace android::drm_hwcomposer {

class DrmCrtc;
class DrmDevice;

struct DstRectInfo;
struct LayerData;

enum class BufferBlendMode;
enum class BufferColorEncoding;
enum class BufferSampleRange;

// NOLINTNEXTLINE(readability-identifier-naming)
struct drm_plane_size_hint_local {
  __u16 width;
  __u16 height;
};

struct DrmColorPipeline {
  std::vector<std::unique_ptr<DrmColorOp>> color_ops{};
  uint64_t gamma_lut_size{};
  uint64_t degamma_lut_size{};
};

class DrmPlane : public PipelineBindable<DrmPlane> {
  friend class FakeDrmPlane;

 public:
  DrmPlane(const DrmPlane &) = delete;
  DrmPlane &operator=(const DrmPlane &) = delete;

  virtual ~DrmPlane() = default;

  static auto CreateInstance(DrmDevice &dev, uint32_t plane_id)
      -> std::unique_ptr<DrmPlane>;

  virtual bool IsCrtcSupported(const DrmCrtc &crtc) const;
  virtual bool IsValidForLayer(const LayerData *layer);

  auto GetType() const {
    return type_;
  }

  bool IsFormatSupported(uint32_t format) const;
  bool HasNonRgbFormat() const;

  auto AtomicSetState(drmModeAtomicReq &pset, LayerData &layer, uint32_t zpos,
                      uint32_t crtc_id, DstRectInfo &whole_display_rect,
                      DrmModeUserPropertyBlobUnique &damage_out) const -> int;
  auto AtomicSetColorPipeline(
      drmModeAtomicReq &pset, DrmModeUserPropertyBlobUnique &ctm_blob,
      DrmModeUserPropertyBlobUnique &degamma_lut_blob,
      DrmModeUserPropertyBlobUnique &gamma_lut_blob) const -> int;
  auto AtomicDisablePlane(drmModeAtomicReq &pset) -> int;
  auto &GetZPosProperty() const {
    return zpos_property_;
  }

  auto GetId() const {
    return plane_->plane_id;
  }

  auto HasColorPipeline() const -> bool {
    return !color_pipeline_.color_ops.empty();
  }
  auto GetDegamma1DLutSize() const {
    return color_pipeline_.degamma_lut_size;
  }
  auto GetGamma1DLutSize() const {
    return color_pipeline_.gamma_lut_size;
  }

 private:
  DrmPlane(DrmDevice &dev, DrmModePlaneUnique plane)
      : drm_(&dev), plane_(std::move(plane)){};
  DrmDevice *const drm_;
  DrmModePlaneUnique plane_;

  enum class Presence { kOptional, kMandatory };

  auto Init() -> int;
  auto GetPlaneProperty(const char *prop_name, DrmProperty &property,
                        Presence presence = Presence::kMandatory) -> bool;

  uint32_t type_{};

  std::vector<uint32_t> formats_;

  DrmProperty crtc_property_;
  DrmProperty fb_property_;
  DrmProperty crtc_x_property_;
  DrmProperty crtc_y_property_;
  DrmProperty crtc_w_property_;
  DrmProperty crtc_h_property_;
  DrmProperty src_x_property_;
  DrmProperty src_y_property_;
  DrmProperty src_w_property_;
  DrmProperty src_h_property_;
  DrmProperty zpos_property_;
  DrmProperty rotation_property_;
  DrmProperty alpha_property_;
  DrmProperty blend_property_;
  DrmProperty in_fence_fd_property_;
  DrmProperty color_encoding_property_;
  DrmProperty color_range_property_;
  DrmProperty size_hints_property_;
  DrmProperty fb_damage_clips_property_;
  DrmProperty color_pipeline_property_;

  std::map<BufferBlendMode, uint64_t> blending_enum_map_;
  std::map<BufferColorEncoding, uint64_t> color_encoding_enum_map_;
  std::map<BufferSampleRange, uint64_t> color_range_enum_map_;
  std::map<uint64_t, ColorOpType> color_op_type_enum_map_;

  DrmColorPipeline color_pipeline_;

  uint64_t transform_enum_mask_ = DRM_MODE_ROTATE_0;
  std::vector<drm_plane_size_hint_local> size_hints_;
};

}  // namespace android::drm_hwcomposer
