#include "IntelBackend.h"

#include <memory>
#include <string>

#include "backend/Backend.h"
#include "backend/BackendManager.h"
#include "backend/GenericBackend.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/IntelMappingValidator.h"

namespace android::drm_hwcomposer {

IntelBackend::IntelBackend(DrmDevice& drm_device) : GenericBackend(drm_device) {
}

std::unique_ptr<CompositionPlanner> IntelBackend::CreateCompositionPlanner() {
  return std::make_unique<GenericLayerMapperCompositionPlanner>(
      IntelValidateMapping);
}

// Register the backend for Intel DRM driver
// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<IntelBackend> kRegisterXe("xe");
// NOLINTNEXTLINE(cert-err58-cpp)
const BackendManager::RegisterBackend<IntelBackend> kRegisterI915("i915");

}  // namespace android::drm_hwcomposer
