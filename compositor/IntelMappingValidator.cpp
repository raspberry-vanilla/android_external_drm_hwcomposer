#include "compositor/IntelMappingValidator.h"

#include <vector>

#include "compositor/LayerData.h"
#include "compositor/mapper/LayerMapper.h"
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {

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
    if (pi.RequireScalingOrPhasing()) {
      const int src_x = static_cast<int>(pi.source_crop.f_rect->left);
      const int src_w = static_cast<int>(pi.source_crop.f_rect->Width());
      const int src_h = static_cast<int>(pi.source_crop.f_rect->Height());
      const int dst_w = pi.display_frame.i_rect->Width();
      const int dst_h = pi.display_frame.i_rect->Height();
      if (src_x % 2 != 0 || src_w % 2 != 0) {
        return false;
      }
      constexpr int kMaxScalerSrcWidth = 4096;
      constexpr int kMaxScalerSrcHeight = 8192;
      constexpr int kMaxScalerDstWidth = 8192;
      constexpr int kMaxScalerDstHeight = 8192;
      if (src_w > kMaxScalerSrcWidth || src_h > kMaxScalerSrcHeight ||
          dst_w > kMaxScalerDstWidth || dst_h > kMaxScalerDstHeight) {
        return false;
      }
      scalers_used++;
    }
  }
  return scalers_used <= kMaxHardwareScalers;
}

}  // namespace android::drm_hwcomposer