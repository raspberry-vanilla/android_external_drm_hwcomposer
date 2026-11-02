/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#if HAS_LIBDISPLAY_INFO
extern "C" {
#include <libdisplay-info/info.h>
}

#include <cstdlib>
#include <memory>

#include "drm/DrmUnique.h"
#endif  // HAS_LIBDISPLAY_INFO

#include "compositor/DisplayInfo.h"

namespace aidl::android::hardware::graphics::common {
enum class Hdr;
}  // namespace aidl::android::hardware::graphics::common

namespace android::ui {
using aidl::android::hardware::graphics::common::Hdr;
}  // namespace android::ui

namespace android::drm_hwcomposer {

// Stub wrapper class for edid parsing
class EdidWrapper {
 public:
  struct VendorProductInfo {
    std::string make;
    std::string model;
    std::string manufacturer;  // PNP manufacturer ID
    uint16_t product_code{};             // EDID product code (bytes 10-11)
    int32_t year;
  };

  EdidWrapper() = default;
  EdidWrapper(const EdidWrapper &) = delete;
  virtual ~EdidWrapper() = default;

  virtual void GetSupportedHdrTypes(std::vector<ui::Hdr> &types) {
    types.clear();
  };
  virtual void GetHdrLuminance(float * /*max_luminance*/,
                               float * /*max_average_luminance*/,
                               float * /*min_luminance*/) {};
  virtual void GetHdrCapabilities(std::vector<ui::Hdr> &types,
                                  float *max_luminance,
                                  float *max_average_luminance,
                                  float *min_luminance) {
    GetSupportedHdrTypes(types);
    GetHdrLuminance(max_luminance, max_average_luminance, min_luminance);
  };
  virtual void GetColorModes(std::vector<ColorMode> &color_modes) {
    color_modes.clear();
  };
  virtual auto GetBoundsMm() -> std::pair<int32_t, int32_t> {
    return {-1, -1};
  }

  virtual VendorProductInfo GetVendorProductInfo() const {
    return VendorProductInfo{};
  }

  virtual std::pair<uint32_t, uint32_t> GetVerticalDisplayRangeLimits() const {
    return {};
  }
};

#if HAS_LIBDISPLAY_INFO
// Wrapper class for that uses libdisplay-info to parse edids
class LibdisplayEdidWrapper final : public EdidWrapper {
 public:
  LibdisplayEdidWrapper() = delete;
  ~LibdisplayEdidWrapper() override {
    di_info_destroy(info_);
  }
  static auto Create(DrmModePropertyBlobUnique blob)
      -> std::unique_ptr<LibdisplayEdidWrapper>;

  void GetSupportedHdrTypes(std::vector<ui::Hdr> &types) override;

  void GetHdrLuminance(float *max_luminance, float *max_average_luminance,
                       float *min_luminance) override;

  void GetHdrCapabilities(std::vector<ui::Hdr> &types,
                          float *max_luminance,
                          float *max_average_luminance,
                          float *min_luminance) override;

  void GetColorModes(std::vector<ColorMode> &color_modes) override;

  auto GetBoundsMm() -> std::pair<int32_t, int32_t> override;

  VendorProductInfo GetVendorProductInfo() const override;

  std::pair<uint32_t, uint32_t> GetVerticalDisplayRangeLimits() const override;

 private:
  friend class LibdisplayEdidWrapperStringOpsTest;

  // libdisplay-info getters that return heap-allocated strings, and the
  // deallocator that releases them. Unit tests substitute these to inject
  // nullptr returns and observe frees without replacing global C symbols.
  struct VendorStringOps {
    char *(*get_make)(const di_info *) = di_info_get_make;
    char *(*get_model)(const di_info *) = di_info_get_model;
    void (*free_string)(void *) = std::free;
  };

  static auto Create(DrmModePropertyBlobUnique blob, VendorStringOps string_ops)
      -> std::unique_ptr<LibdisplayEdidWrapper>;

  LibdisplayEdidWrapper(di_info *info, VendorStringOps string_ops)
      : info_(info), string_ops_(string_ops) {
  }

  di_info *info_{};
  VendorStringOps string_ops_;
};
#endif

}  // namespace android::drm_hwcomposer
