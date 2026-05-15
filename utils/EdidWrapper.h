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
    int32_t year;
  };

  EdidWrapper() = default;
  EdidWrapper(const EdidWrapper &) = delete;
  virtual ~EdidWrapper() = default;

  virtual void GetSupportedHdrTypes(std::vector<ui::Hdr> &types) {
    types.clear();
  };
  virtual void GetHdrCapabilities(std::vector<ui::Hdr> &types,
                                  float * /*max_luminance*/,
                                  float * /*max_average_luminance*/,
                                  float * /*min_luminance*/) {
    GetSupportedHdrTypes(types);
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

  void GetHdrCapabilities(std::vector<ui::Hdr> &types,
                          float *max_luminance,
                          float *max_average_luminance,
                          float *min_luminance) override;

  void GetColorModes(std::vector<ColorMode> &color_modes) override;

  auto GetBoundsMm() -> std::pair<int32_t, int32_t> override;

  VendorProductInfo GetVendorProductInfo() const override;

 private:
  LibdisplayEdidWrapper(di_info *info) : info_(std::move(info)) {
  }

  di_info *info_{};
};
#endif

}  // namespace android::drm_hwcomposer
