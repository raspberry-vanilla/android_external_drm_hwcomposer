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

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "utils/BacklightController.h"
#include "utils/BacklightFileInterface.h"

namespace android::drm_hwcomposer {

// An instance of BacklightController which uses backlights located in sysfs
// at /sys/class/backlight/*
class SysfsBacklightController : public BacklightController {
 public:
  ~SysfsBacklightController() override = default;

  // Returns a set of backlight names from sysfs. These names can be passed to
  // CreateInstanceFromName to create a controller for that backlight.
  static auto EnumerateBacklights() -> std::set<std::string>;

  // Creates a controller for the backlight with the given name. Returns nullptr
  // if the backlight could not be found or initialized.
  static auto CreateInstanceFromName(
      const std::string &name,
      std::unique_ptr<BacklightFileInterface> file_intf = nullptr)
      -> std::unique_ptr<BacklightController>;

  SysfsBacklightController(const SysfsBacklightController &) = delete;
  SysfsBacklightController &operator=(const SysfsBacklightController &) =
      delete;

  bool SetBrightness(std::optional<float> brightness) override;
  auto GetName() const -> std::string override {
    return name_;
  }

 private:
  SysfsBacklightController(std::string name, std::string path, bool powered,
                           float max, bool hw_handles_encoding,
                           std::unique_ptr<BacklightFileInterface> file_intf)
      : name_(std::move(name)),
        sysfs_path_(std::move(path)),
        powered_(powered),
        max_(max),
        hw_handles_encoding_(hw_handles_encoding),
        file_intf_(std::move(file_intf)) {
  }

  std::string name_;
  std::string sysfs_path_;
  bool powered_;
  float max_;
  bool hw_handles_encoding_;
  std::unique_ptr<BacklightFileInterface> file_intf_;
};

}  // namespace android::drm_hwcomposer
