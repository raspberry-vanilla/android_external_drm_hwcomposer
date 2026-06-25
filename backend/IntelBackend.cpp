#include "IntelBackend.h"

#include <memory>
#include <string>

#include "backend/GenericBackend.h"
#include "compositor/GenericLayerMapperCompositionPlanner.h"
#include "compositor/IntelMappingValidator.h"

namespace android::drm_hwcomposer {

IntelBackend::IntelBackend(const std::string& name) : GenericBackend(name) {
}

std::unique_ptr<CompositionPlanner> IntelBackend::CreateCompositionPlanner() {
  return std::make_unique<GenericLayerMapperCompositionPlanner>(
      IntelValidateMapping);
}

// Register the backend for Intel DRM drivers
// NOLINTNEXTLINE(cert-err58-cpp)
static const IntelBackend kXeBackend("xe");
// NOLINTNEXTLINE(cert-err58-cpp)
static const IntelBackend kI915Backend("i915");

}  // namespace android::drm_hwcomposer