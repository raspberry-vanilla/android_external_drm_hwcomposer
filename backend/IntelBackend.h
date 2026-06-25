#pragma once

#include <memory>
#include <string>

#include "backend/GenericBackend.h"

namespace android::drm_hwcomposer {

class CompositionPlanner;

class IntelBackend : public GenericBackend {
 public:
  explicit IntelBackend(const std::string& name);
  ~IntelBackend() override = default;

 protected:
  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override;
};

}  // namespace android::drm_hwcomposer