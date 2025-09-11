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

#define LOG_TAG "drmhwc"

#include "DrmPlane.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>

#include "DrmDevice.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/LayerData.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {
// Ensure that |src| does not exceed the bounds of the buffer.
void ClipSourceCrop(FRect &src, const BufferInfo &buffer_info) {
  src.left = std::max(src.left, 0.F);
  src.top = std::max(src.top, 0.F);
  src.right = std::min(src.right, static_cast<float>(buffer_info.width));
  src.bottom = std::min(src.bottom, static_cast<float>(buffer_info.height));
}
}  // namespace

auto DrmPlane::CreateInstance(DrmDevice &dev, uint32_t plane_id)
    -> std::unique_ptr<DrmPlane> {
  auto p = MakeDrmModePlaneUnique(*dev.GetFd(), plane_id);
  if (!p) {
    ALOGE("Failed to get plane %d", plane_id);
    return {};
  }

  auto plane = std::unique_ptr<DrmPlane>(new DrmPlane(dev, std::move(p)));

  if (plane->Init() != 0) {
    ALOGE("Failed to init plane %d", plane_id);
    return {};
  }

  return plane;
}

int DrmPlane::Init() {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  formats_ = {plane_->formats, plane_->formats + plane_->count_formats};

  DrmProperty p;

  if (!GetPlaneProperty("type", p)) {
    return -ENOTSUP;
  }

  auto type = p.GetValue();
  if (!type) {
    ALOGE("Failed to get plane type property value");
    return -EINVAL;
  }
  switch (*type) {
    case DRM_PLANE_TYPE_OVERLAY:
    case DRM_PLANE_TYPE_PRIMARY:
    case DRM_PLANE_TYPE_CURSOR:
      type_ = (uint32_t)*type;
      break;
    default:
      ALOGE("Invalid plane type %" PRIu64, *type);
      return -EINVAL;
  }

  if (!GetPlaneProperty("CRTC_ID", crtc_property_) ||
      !GetPlaneProperty("FB_ID", fb_property_) ||
      !GetPlaneProperty("CRTC_X", crtc_x_property_) ||
      !GetPlaneProperty("CRTC_Y", crtc_y_property_) ||
      !GetPlaneProperty("CRTC_W", crtc_w_property_) ||
      !GetPlaneProperty("CRTC_H", crtc_h_property_) ||
      !GetPlaneProperty("SRC_X", src_x_property_) ||
      !GetPlaneProperty("SRC_Y", src_y_property_) ||
      !GetPlaneProperty("SRC_W", src_w_property_) ||
      !GetPlaneProperty("SRC_H", src_h_property_)) {
    return -ENOTSUP;
  }

  GetPlaneProperty("zpos", zpos_property_, Presence::kOptional);

  if (GetPlaneProperty("rotation", rotation_property_, Presence::kOptional)) {
    rotation_property_.GetEnumMask(transform_enum_mask_);
  }

  GetPlaneProperty("alpha", alpha_property_, Presence::kOptional);

  if (GetPlaneProperty("pixel blend mode", blend_property_,
                       Presence::kOptional)) {
    blend_property_.AddEnumToMap("Pre-multiplied", BufferBlendMode::kPreMult,
                                 blending_enum_map_);
    blend_property_.AddEnumToMap("Coverage", BufferBlendMode::kCoverage,
                                 blending_enum_map_);
    blend_property_.AddEnumToMap("None", BufferBlendMode::kNone,
                                 blending_enum_map_);
  }

  GetPlaneProperty("IN_FENCE_FD", in_fence_fd_property_, Presence::kOptional);

  if (HasNonRgbFormat()) {
    if (GetPlaneProperty("COLOR_ENCODING", color_encoding_property_,
                         Presence::kOptional)) {
      color_encoding_property_.AddEnumToMap("ITU-R BT.709 YCbCr",
                                            BufferColorSpace::kItuRec709,
                                            color_encoding_enum_map_);
      color_encoding_property_.AddEnumToMap("ITU-R BT.601 YCbCr",
                                            BufferColorSpace::kItuRec601,
                                            color_encoding_enum_map_);
      color_encoding_property_.AddEnumToMap("ITU-R BT.2020 YCbCr",
                                            BufferColorSpace::kItuRec2020,
                                            color_encoding_enum_map_);
    }

    if (GetPlaneProperty("COLOR_RANGE", color_range_property_,
                         Presence::kOptional)) {
      color_range_property_.AddEnumToMap("YCbCr full range",
                                         BufferSampleRange::kFullRange,
                                         color_range_enum_map_);
      color_range_property_.AddEnumToMap("YCbCr limited range",
                                         BufferSampleRange::kLimitedRange,
                                         color_range_enum_map_);
    }
  }

  if (type_ == DRM_PLANE_TYPE_CURSOR &&
      GetPlaneProperty("SIZE_HINTS", size_hints_property_,
                       Presence::kOptional)) {
    size_hints_property_.GetBlobData(size_hints_);
  }

  GetPlaneProperty("FB_DAMAGE_CLIPS", fb_damage_clips_property_,
                   Presence::kOptional);

  return 0;
}

bool DrmPlane::IsCrtcSupported(const DrmCrtc &crtc) const {
  auto crtc_prop_optval = crtc_property_.GetValue();
  auto crtc_prop_val = crtc_prop_optval ? *crtc_prop_optval : 0;

  if (crtc_prop_val != 0 && crtc_prop_val != crtc.GetId() &&
      GetType() == DRM_PLANE_TYPE_PRIMARY) {
    // Some DRM driver such as omap_drm allows sharing primary plane between
    // CRTCs, but the primary plane could not be shared if it has been used by
    // any CRTC already, which is protected by the plane_switching_crtc function
    // in the kernel drivers/gpu/drm/drm_atomic.c file.
    // The current drm_hwc design is not ready to support such scenario yet,
    // so adding the CRTC status check here to workaround for now.
    return false;
  }

  return ((1 << crtc.GetIndexInResArray()) & plane_->possible_crtcs) != 0;
}

static uint64_t ToDrmRotation(LayerTransform transform) {
  /* DRM/KMS uses counter-clockwise rotations, while HWC API uses
   * clockwise. That's why 90 and 270 are swapped here.
   */
  uint64_t rotation = DRM_MODE_ROTATE_0;

  if (transform.rotate90) {
    rotation |= DRM_MODE_ROTATE_270;
  }

  if (transform.hflip) {
    rotation |= DRM_MODE_REFLECT_X;
  }

  if (transform.vflip) {
    rotation |= DRM_MODE_REFLECT_Y;
  }

  // TODO(nobody): Respect transform_enum_mask_ to find alternative rotation
  // values

  return rotation;
}

bool DrmPlane::IsValidForLayer(const LayerData *layer) {
  if (layer == nullptr || !layer->bi) {
    ALOGE("%s: Invalid parameters", __func__);
    return false;
  }

  uint64_t drm_rotation = ToDrmRotation(layer->pi.transform);
  if ((drm_rotation & transform_enum_mask_) != drm_rotation) {
    ALOGV("Transform is not supported on plane %d", GetId());
    return false;
  }

  if (!alpha_property_ && layer->pi.alpha != kAlphaOpaque) {
    ALOGV("Alpha is not supported on plane %d", GetId());
    return false;
  }

  if (blending_enum_map_.count(layer->bi->blend_mode) == 0 &&
      layer->bi->blend_mode != BufferBlendMode::kNone &&
      layer->bi->blend_mode != BufferBlendMode::kPreMult) {
    ALOGV("Blending is not supported on plane %d", GetId());
    return false;
  }

  auto format = layer->bi->format;
  if (!IsFormatSupported(format)) {
    ALOGV("Plane %d does not supports %c%c%c%c format", GetId(), format,
          format >> 8, format >> 16, format >> 24);
    return false;
  }

  return true;
}

bool DrmPlane::IsFormatSupported(uint32_t format) const {
  return std::find(std::begin(formats_), std::end(formats_), format) !=
         std::end(formats_);
}

bool DrmPlane::HasNonRgbFormat() const {
  return std::find_if_not(std::begin(formats_), std::end(formats_),
                          [](uint32_t format) {
                            return BufferInfoGetter::IsDrmFormatRgb(format);
                          }) != std::end(formats_);
}

/* Convert float to 16.16 fixed point */
static int To1616FixPt(float in) {
  constexpr int kBitShift = 16;
  return int(in * (1 << kBitShift));
}

// NOLINTNEXTLINE (readability-function-cognitive-complexity)
auto DrmPlane::AtomicSetState(drmModeAtomicReq &pset, LayerData &layer,
                              uint32_t zpos, uint32_t crtc_id,
                              DstRectInfo &whole_display_rect,
                              DrmModeUserPropertyBlobUnique &damage_out) const
    -> int {
  if (!layer.fb || !layer.bi) {
    ALOGE("%s: Invalid arguments", __func__);
    return -EINVAL;
  }

  if (zpos_property_ && !zpos_property_.IsImmutable()) {
    uint64_t min_zpos = 0;

    // Ignore ret and use min_zpos as 0 by default
    std::tie(std::ignore, min_zpos) = zpos_property_.RangeMin();

    if (!zpos_property_.AtomicSet(pset, zpos + min_zpos)) {
      return -EINVAL;
    }
  }

  if (layer.acquire_fence &&
      !in_fence_fd_property_.AtomicSet(pset, *layer.acquire_fence)) {
    return -EINVAL;
  }

  auto opt_disp = layer.pi.display_frame.i_rect;
  if (!layer.pi.display_frame.i_rect) {
    opt_disp = whole_display_rect.i_rect;
  }

  auto opt_src = layer.pi.source_crop.f_rect;
  if (!layer.pi.source_crop.f_rect) {
    opt_src = {0.0F, 0.0F, float(layer.bi->width), float(layer.bi->height)};
  }

  if (!opt_disp || !opt_src) {
    ALOGE("%s: Invalid display frame or source crop", __func__);
    return -EINVAL;
  }

  auto disp = opt_disp.value();
  auto src = opt_src.value();

  if (type_ == DRM_PLANE_TYPE_CURSOR) {
    // Calculate scaling factors in each direction so that they can be
    // preserved.
    const float hscale = src.Width() / static_cast<float>(disp.Width());
    const float vscale = src.Height() / static_cast<float>(disp.Height());
    // Panning (i.e. non-zero src position) is not permitted with cursor plane.
    // Shift the display frame in the opposite direction to position the cursor
    // correctly. Then clear the src position.
    disp.left -= static_cast<int>(src.left);
    disp.top -= static_cast<int>(src.top);
    src.left = 0.0F;
    src.top = 0.0F;
    // Force the display frame to occupy the full buffer, so that its size is
    // known to be compatible with cursor plane restrictions.
    disp.right = disp.left + static_cast<int>(layer.bi->width);
    disp.bottom = disp.top + static_cast<int>(layer.bi->height);
    // Resize the src rect to preserve the original scaling factor relative to
    // the new disp size.
    src.right = hscale * static_cast<float>(disp.Width());
    src.bottom = vscale * static_cast<float>(disp.Height());
  }

  // Clip the source crop rect to ensure it does not exceed the bounds of the
  // framebuffer.
  ClipSourceCrop(src, *layer.bi);

  if (!crtc_property_.AtomicSet(pset, crtc_id) ||
      !fb_property_.AtomicSet(pset, layer.fb->GetFbId()) ||
      !crtc_x_property_.AtomicSet(pset, disp.left) ||
      !crtc_y_property_.AtomicSet(pset, disp.top) ||
      !crtc_w_property_.AtomicSet(pset, disp.Width()) ||
      !crtc_h_property_.AtomicSet(pset, disp.Height()) ||
      !src_x_property_.AtomicSet(pset, To1616FixPt(src.left)) ||
      !src_y_property_.AtomicSet(pset, To1616FixPt(src.top)) ||
      !src_w_property_.AtomicSet(pset, To1616FixPt(src.Width())) ||
      !src_h_property_.AtomicSet(pset, To1616FixPt(src.Height()))) {
    return -EINVAL;
  }

  if (rotation_property_ &&
      !rotation_property_.AtomicSet(pset, ToDrmRotation(layer.pi.transform))) {
    return -EINVAL;
  }

  if (alpha_property_ &&
      !alpha_property_.AtomicSet(pset,
                                 std::lround(layer.pi.alpha * UINT16_MAX))) {
    return -EINVAL;
  }

  if (blending_enum_map_.count(layer.bi->blend_mode) != 0 &&
      !blend_property_.AtomicSet(pset,
                                 blending_enum_map_.at(layer.bi->blend_mode))) {
    return -EINVAL;
  }

  if (color_encoding_enum_map_.count(layer.bi->color_space) != 0 &&
      !color_encoding_property_.AtomicSet(pset, color_encoding_enum_map_.at(
                                                    layer.bi->color_space))) {
    return -EINVAL;
  }

  if (color_range_enum_map_.count(layer.bi->sample_range) != 0 &&
      !color_range_property_.AtomicSet(pset, color_range_enum_map_.at(
                                                 layer.bi->sample_range))) {
    return -EINVAL;
  }

  if (fb_damage_clips_property_) {
    std::vector<drm_mode_rect> plane_damage;
    for (const auto &rect : layer.pi.damage.dmg_rects) {
      if (rect.left == rect.right || rect.top == rect.bottom) {
        // SurfaceFlinger uses empty rects to signal no damage, but kernel
        // doesn't support this.
        continue;
      }
      plane_damage.emplace_back(drm_mode_rect{.x1 = rect.left,
                                              .y1 = rect.top,
                                              .x2 = rect.right,
                                              .y2 = rect.bottom});
    }

    if (!plane_damage.empty()) {
      size_t damage_size = sizeof(drm_mode_rect) * plane_damage.size();
      damage_out = drm_->RegisterUserPropertyBlob(plane_damage.data(),
                                                  damage_size);
      if (!damage_out ||
          !fb_damage_clips_property_.AtomicSet(pset, *damage_out)) {
        ALOGE("%s: Failed to set %s property", __func__,
              fb_damage_clips_property_.GetName().c_str());
        // Continue without returning error code. FB_DAMAGE_CLIPS is an optional
        // property. Default behavior is to assume full plane damage.
      }
    }
  }

  return 0;
}

auto DrmPlane::AtomicDisablePlane(drmModeAtomicReq &pset) -> int {
  if (!crtc_property_.AtomicSet(pset, 0) || !fb_property_.AtomicSet(pset, 0)) {
    return -EINVAL;
  }

  return 0;
}

auto DrmPlane::GetPlaneProperty(const char *prop_name, DrmProperty &property,
                                Presence presence) -> bool {
  auto err = drm_->GetProperty(GetId(), DRM_MODE_OBJECT_PLANE, prop_name,
                               &property);
  if (err != 0) {
    if (presence == Presence::kMandatory) {
      ALOGE("Could not get mandatory property \"%s\" from plane %d", prop_name,
            GetId());
    } else {
      ALOGV("Could not get optional property \"%s\" from plane %d", prop_name,
            GetId());
    }
    return false;
  }

  return true;
}

}  // namespace android::drm_hwcomposer
