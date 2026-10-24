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

#if HAS_LIBDISPLAY_INFO
extern "C" {
#include <libdisplay-info/cta.h>
#include <libdisplay-info/displayid.h>
#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>
}
#ifdef ANDROID
#include <ui/GraphicTypes.h>
#else
// <ui/GraphicTypes.h> and <android/hardware/graphics/common/1.1/types.h> pull
// in <hidl/HidlInternal.h>, which fails a static_assert on sizeof(std::string)
// when compiled against host GNU libstdc++ in drm_hwcomposer_unittests.
namespace aidl::android::hardware::graphics::common {
enum class Hdr : int32_t {
  INVALID = 0,
  DOLBY_VISION = 1,
  HDR10 = 2,
  HLG = 3,
  HDR10_PLUS = 4,
  DOLBY_VISION_4K30 = 5,
  HLG_PLUS = 6,
};
}  // namespace aidl::android::hardware::graphics::common
#endif

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "drm/DrmUnique.h"

#include "utils/EdidWrapper.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

auto LibdisplayEdidWrapper::Create(DrmModePropertyBlobUnique blob)
    -> std::unique_ptr<LibdisplayEdidWrapper> {
  if (!blob)
    return nullptr;

  auto *info = di_info_parse_edid(blob->data, blob->length);
  if (!info) {
    ALOGW("Failed to parse edid blob.");
    return nullptr;
  }

  return std::unique_ptr<LibdisplayEdidWrapper>(
      new LibdisplayEdidWrapper(std::move(info)));
}

void LibdisplayEdidWrapper::GetSupportedHdrTypes(std::vector<ui::Hdr> &types) {
  types.clear();

  const auto *hdr_static_meta = di_info_get_hdr_static_metadata(info_);
  const auto *colorimetries = di_info_get_supported_signal_colorimetry(info_);
  if (colorimetries->bt2020_cycc || colorimetries->bt2020_ycc ||
      colorimetries->bt2020_rgb) {
    if (hdr_static_meta->pq)
      types.emplace_back(ui::Hdr::HDR10);
    if (hdr_static_meta->hlg)
      types.emplace_back(ui::Hdr::HLG);
  }
}

void LibdisplayEdidWrapper::GetHdrLuminance(float *max_luminance,
                                            float *max_average_luminance,
                                            float *min_luminance) {
  const auto *hdr_static_meta = di_info_get_hdr_static_metadata(info_);
  *max_luminance = hdr_static_meta->desired_content_max_luminance;
  *max_average_luminance = hdr_static_meta
                               ->desired_content_max_frame_avg_luminance;
  *min_luminance = hdr_static_meta->desired_content_min_luminance;
}

void LibdisplayEdidWrapper::GetHdrCapabilities(std::vector<ui::Hdr> &types,
                                               float *max_luminance,
                                               float *max_average_luminance,
                                               float *min_luminance) {
  GetSupportedHdrTypes(types);
  GetHdrLuminance(max_luminance, max_average_luminance, min_luminance);
}

void LibdisplayEdidWrapper::GetColorModes(std::vector<ColorMode> &color_modes) {
  color_modes.clear();
  color_modes.emplace_back(ColorMode::kNative);

  const auto *hdr_static_meta = di_info_get_hdr_static_metadata(info_);
  const auto *colorimetries = di_info_get_supported_signal_colorimetry(info_);

  /* Rec. ITU-R BT.2020 constant luminance YCbCr */
  /* Rec. ITU-R BT.2020 non-constant luminance YCbCr */
  if (colorimetries->bt2020_cycc || colorimetries->bt2020_ycc)
    color_modes.emplace_back(ColorMode::kBt2020);

  /* Rec. ITU-R BT.2020 RGB */
  if (colorimetries->bt2020_rgb)
    color_modes.emplace_back(ColorMode::kDisplayBt2020);

  /* SMPTE ST 2113 RGB: P3D65 and P3DCI */
  if (colorimetries->st2113_rgb) {
    color_modes.emplace_back(ColorMode::kDciP3);
    color_modes.emplace_back(ColorMode::kDisplayP3);
  }

  /* Rec. ITU-R BT.2100 ICtCp HDR (with PQ and/or HLG) */
  if (colorimetries->ictcp) {
    if (hdr_static_meta->pq)
      color_modes.emplace_back(ColorMode::kBt2100Pq);
    if (hdr_static_meta->hlg)
      color_modes.emplace_back(ColorMode::kBt2100Hlg);
  }
}

auto LibdisplayEdidWrapper::GetBoundsMm() -> std::pair<int32_t, int32_t> {
  const auto *edid = di_info_get_edid(info_);
  if (edid == nullptr) {
    return {-1, -1};
  }

  const auto *const *detailed_timing_defs = di_edid_get_detailed_timing_defs(
      edid);
  for (size_t i = 0; detailed_timing_defs[i] != nullptr; ++i) {
    const auto *dtd = detailed_timing_defs[i];
    if (dtd->horiz_image_mm > 0 && dtd->vert_image_mm > 0) {
      return {dtd->horiz_image_mm, dtd->vert_image_mm};
    }
  }

  const auto *const *exts = di_edid_get_extensions(edid);
  for (size_t i = 0; exts[i] != nullptr; ++i) {
    const auto *cta = di_edid_ext_get_cta(exts[i]);
    if (cta != nullptr) {
      const auto *const *cta_dtds = di_edid_cta_get_detailed_timing_defs(cta);
      for (size_t j = 0; cta_dtds[j] != nullptr; ++j) {
        if (cta_dtds[j]->horiz_image_mm > 0 && cta_dtds[j]->vert_image_mm > 0) {
          return {cta_dtds[j]->horiz_image_mm, cta_dtds[j]->vert_image_mm};
        }
      }
    }
    const auto *did = di_edid_ext_get_displayid(exts[i]);
    if (did != nullptr) {
      const auto *const *blocks = di_displayid_get_data_blocks(did);
      for (size_t j = 0; blocks[j] != nullptr; ++j) {
        const auto *params = di_displayid_data_block_get_display_params(
            blocks[j]);
        if (params != nullptr && params->horiz_image_mm > 0 &&
            params->vert_image_mm > 0) {
          return {static_cast<int32_t>(std::lround(params->horiz_image_mm)),
                  static_cast<int32_t>(std::lround(params->vert_image_mm))};
        }
      }
    }
  }

  // Try to fallback on display size if no DTD or extension block specifies it.
  // However since EDID screen size is vastly unreliable only provide a valid
  // width to avoid invalid DPI computation. When width_cm is also 0 (e.g.
  // projectors or undefined physical size per VESA EDID 1.4 section 3.6.2),
  // return {0, 0} rather than mixing a 0 width with an error (-1) height.
  const auto *screen_size = di_edid_get_screen_size(edid);
  if (screen_size == nullptr || screen_size->width_cm <= 0) {
    return {0, 0};
  }
  return {screen_size->width_cm * 10, -1};
}

EdidWrapper::VendorProductInfo LibdisplayEdidWrapper::GetVendorProductInfo()
    const {
  auto vendor = VendorProductInfo{.make = std::string(di_info_get_make(info_)),
                                  .model = std::string(
                                      di_info_get_model(info_))};

  const auto *edid = di_info_get_edid(info_);
  if (edid != nullptr) {
    const auto *vendor_product = di_edid_get_vendor_product(edid);
    if (vendor_product != nullptr) {
      vendor.year = vendor_product->manufacture_year;
      vendor.manufacturer = vendor_product->manufacturer;
      vendor.product_code = vendor_product->product;
    }
  }

  return vendor;
}

std::pair<uint32_t, uint32_t>
LibdisplayEdidWrapper::GetVerticalDisplayRangeLimits() const {
  const auto *edid = di_info_get_edid(info_);
  if (edid != nullptr) {
    const auto *const *desc = di_edid_get_display_descriptors(edid);
    for (size_t i = 0; desc[i] != nullptr; ++i) {
      const auto *limits = di_edid_display_descriptor_get_range_limits(desc[i]);
      if (limits != nullptr) {
        return {limits->min_vert_rate_hz, limits->max_vert_rate_hz};
      }
    }
  }
  return {};
}

}  // namespace android::drm_hwcomposer

#endif
