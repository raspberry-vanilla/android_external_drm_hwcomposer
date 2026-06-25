#pragma once

#include <vector>

#include "compositor/mapper/LayerMapper.h"

namespace android::drm_hwcomposer {

bool IntelValidateMapping(const std::vector<LayerMapping>& test_layers);

}  // namespace android::drm_hwcomposer