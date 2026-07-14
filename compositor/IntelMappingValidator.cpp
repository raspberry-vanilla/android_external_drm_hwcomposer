#include "compositor/IntelMappingValidator.h"

#include <cstdint>
#include <vector>

#include <drm/drm_fourcc.h>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

static bool IsYuvFormat(uint32_t format) {
  switch (format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61:
    case DRM_FORMAT_YUV410:
    case DRM_FORMAT_YVU410:
    case DRM_FORMAT_YUV411:
    case DRM_FORMAT_YVU411:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YVU422:
    case DRM_FORMAT_YUV444:
    case DRM_FORMAT_YVU444:
      return true;
    default:
      return false;
  }
}

bool IntelValidateMapping(const std::vector<LayerMapping>& test_layers) {
  constexpr int kMaxHardwareScalers = 2;
  int scalers_used = 0;
  for (const auto& mapping : test_layers) {
    if (mapping.composition_type == CompositionType::kClient ||
        // Invalid layers should be treated as client composited to be
        // conservative.
        mapping.composition_type == CompositionType::kInvalid) {
      continue;
    }
    const auto& pi = mapping.layer->GetLayerData().pi;
    // Intel hardware does not support 90/270 degree rotation on any layer.
    if (pi.transform.rotate90) {
      return false;
    }

    const int src_x = static_cast<int>(pi.source_crop.f_rect->left);
    const int src_w = static_cast<int>(pi.source_crop.f_rect->Width());
    const int src_h = static_cast<int>(pi.source_crop.f_rect->Height());
    const int dst_w = pi.display_frame.i_rect->Width();
    const int dst_h = pi.display_frame.i_rect->Height();
    const auto& layer_data = mapping.layer->GetLayerData();

    constexpr int kMinSrcHeight = 1;
    if (src_h < kMinSrcHeight) {
      return false;
    }

    if (layer_data.bi) {
      constexpr int kMinNv12SrcWidth = 16;
      if (layer_data.bi->format == DRM_FORMAT_NV12 && src_w < kMinNv12SrcWidth) {
        return false;
      }
      constexpr int kMinXrgbSrcWidth = 4;
      if (layer_data.bi->format == DRM_FORMAT_XRGB8888 && src_w < kMinXrgbSrcWidth) {
        return false;
      }
    }

    if (pi.RequireScalingOrPhasing()) {
      constexpr int kMinScalerSrcWidth = 8;
      constexpr int kMinYuvScalerSrcHeight = 16;
      constexpr int kMinRgbScalerSrcHeight = 8;
      if (layer_data.bi && IsYuvFormat(layer_data.bi->format)) {
        if (src_w < kMinScalerSrcWidth || src_h < kMinYuvScalerSrcHeight) {
          return false;
        }
      } else {
        if (src_w < kMinScalerSrcWidth || src_h < kMinRgbScalerSrcHeight) {
          return false;
        }
      }

      constexpr int kMaxUpscaleFactor = 32768;  // 2^15
      if (dst_w > src_w * kMaxUpscaleFactor ||
          dst_h > src_h * kMaxUpscaleFactor) {
        return false;
      }

      if (scalers_used == 0) {
        if (src_w > dst_w * 3 || src_h > dst_h * 3) {
          return false;
        }
      } else if (scalers_used == 1) {
        if (layer_data.bi && IsYuvFormat(layer_data.bi->format)) {
          return false;
        }
        if (src_w > dst_w || src_h > dst_h) {
          return false;
        }
      }

      if (src_x % 2 != 0 || src_w % 2 != 0) {
        return false;
      }

      constexpr int kMaxRgbScalerSrcWidth = 6144;
      constexpr int kMaxYuvScalerSrcWidth = 4096;
      int max_scaler_src_width = kMaxRgbScalerSrcWidth;
      if (layer_data.bi && IsYuvFormat(layer_data.bi->format)) {
        max_scaler_src_width = kMaxYuvScalerSrcWidth;
      }

      constexpr int kMaxScalerSrcHeight = 8192;
      constexpr int kMaxScalerDstWidth = 8192;
      constexpr int kMaxScalerDstHeight = 8192;
      if (src_w > max_scaler_src_width || src_h > kMaxScalerSrcHeight ||
          dst_w > kMaxScalerDstWidth || dst_h > kMaxScalerDstHeight) {
        return false;
      }
      scalers_used++;
    }
  }
  return scalers_used <= kMaxHardwareScalers;
}

}  // namespace android::drm_hwcomposer