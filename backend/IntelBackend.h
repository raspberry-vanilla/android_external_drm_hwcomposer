#pragma once

#include <memory>

#include "backend/Backend.h"
#include "backend/GenericBackend.h"

namespace android::drm_hwcomposer {

class CompositionPlanner;

class IntelBackend : public GenericBackend {
 public:
  explicit IntelBackend(DrmDevice& drm_device);
  ~IntelBackend() override = default;

 protected:
  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override;
};

}  // namespace android::drm_hwcomposer
