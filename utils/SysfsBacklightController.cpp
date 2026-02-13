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

#define LOG_TAG "drmhwc"

#include "SysfsBacklightController.h"

#include <limits>
#include <linux/fb.h>
#include <set>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "utils/log.h"

// NOLINTNEXTLINE(cert-err58-cpp,warnings-as-errors)
static const std::string kBasePath = "/sys/class/backlight";

namespace android::drm_hwcomposer {

namespace {

class FsBacklightFileInterface : public BacklightFileInterface {
 public:
  bool ReadFileToString(const std::string &path,
                        std::string *content) override {
    return ::android::base::ReadFileToString(path, content);
  }
  bool WriteStringToFile(const std::string &content,
                         const std::string &path) override {
    return ::android::base::WriteStringToFile(content, path);
  }
};

}  // namespace

auto SysfsBacklightController::EnumerateBacklights() -> std::set<std::string> {
  std::set<std::string> ret;
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(kBasePath.c_str()),
                                                closedir);
  if (!dir) {
    ALOGE("Could not open the backlight sysfs node (%d)", errno);
    return ret;
  }

  dirent *de = nullptr;
  // NOLINTNEXTLINE(concurrency-mt-unsafe) - DIR stream is function-scoped
  while ((de = readdir(dir.get())) != nullptr) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    ret.emplace(de->d_name);
  }
  return ret;
}

auto SysfsBacklightController::CreateInstanceFromName(
    const std::string &name, std::unique_ptr<BacklightFileInterface> file_intf)
    -> std::unique_ptr<BacklightController> {
  if (!file_intf) {
    file_intf = std::make_unique<FsBacklightFileInterface>();
  }

  std::ostringstream path;
  path << kBasePath << "/" << name << "/";

  std::string power_path(path.str());
  power_path += "bl_power";
  std::string file_contents;
  if (!file_intf->ReadFileToString(power_path, &file_contents)) {
    ALOGE("Could not read power from %s", power_path.c_str());
    return nullptr;
  }

  file_contents = ::android::base::Trim(file_contents);
  int power = 0;
  if (!::android::base::ParseInt(file_contents, &power)) {
    ALOGE("Could not parse power (%s) from %s (%d)", file_contents.c_str(),
          power_path.c_str(), errno);
    return nullptr;
  }

  bool powered = false;
  if (power == FB_BLANK_UNBLANK) {
    powered = true;
  } else if (power == FB_BLANK_POWERDOWN) {
    powered = false;
  } else {
    ALOGE("Read undefined value from bl_power (%s) = %d", power_path.c_str(),
          power);
  }

  std::string max_brightness_path(path.str());
  max_brightness_path += "max_brightness";
  if (!file_intf->ReadFileToString(max_brightness_path, &file_contents)) {
    ALOGE("Could not read max brightness from %s", max_brightness_path.c_str());
    return nullptr;
  }

  file_contents = ::android::base::Trim(file_contents);
  int max = 0;
  if (!::android::base::ParseInt(file_contents, &max)) {
    ALOGE("Could not parse max brightness (%s) from %s (%d)",
          file_contents.c_str(), max_brightness_path.c_str(), errno);
    return nullptr;
  }
  if (max >= static_cast<int>(std::numeric_limits<float>::max()) - 1) {
    ALOGE("Max brightness is too large (%s) from %s (%d)",
          file_contents.c_str(), max_brightness_path.c_str(), errno);
    return nullptr;
  }

  std::string scale_path(path.str());
  scale_path += "scale";
  if (!file_intf->ReadFileToString(scale_path, &file_contents)) {
    ALOGE("Could not read backlight scale from %s (%d)", scale_path.c_str(),
          errno);
    return nullptr;
  }
  file_contents = ::android::base::Trim(file_contents);

  // If the scale is "linear", the hardware handles the perceptual encoding.
  // Otherwise (unknown or "non-linear"), the hardware is "passthrough" and we must
  // apply the HLG OETF in the HAL.
  bool hw_handles_encoding = file_contents == "linear";

  ALOGI("Backlight %s (powered=%s max=%d %s encoding)", path.str().c_str(),
        powered ? "yes" : "no", max,
        hw_handles_encoding ? "hw" : "hal applies");

  return std::unique_ptr<BacklightController>(
      new SysfsBacklightController(name, path.str(), powered,
                                   static_cast<float>(max), hw_handles_encoding,
                                   std::move(file_intf)));
}

auto SysfsBacklightController::SetBrightness(std::optional<float> brightness)
    -> bool {
  std::string power_path(sysfs_path_);
  power_path += "bl_power";

  if (brightness == std::nullopt) {
    if (powered_) {
      powered_ = false;
      if (!file_intf_->WriteStringToFile(std::to_string(FB_BLANK_POWERDOWN),
                                         power_path)) {
        ALOGW("Failed to turn backlight off at %s (%d)", power_path.c_str(),
              errno);
      }
    }
    return true;
  }

  if (*brightness < kMin || *brightness > kMax) {
    ALOGE("Brightness value %f is out of range [0, 1]", *brightness);
    return false;
  }

  if (!powered_) {
    if (!file_intf_->WriteStringToFile(std::to_string(FB_BLANK_UNBLANK),
                                       power_path)) {
      ALOGW("Failed to turn backlight on at %s (%d)", power_path.c_str(),
            errno);
    }
    powered_ = true;
  }

  float val = hw_handles_encoding_ ? *brightness : HlgOetf(*brightness);

  int normalized = static_cast<int>(max_ * val);
  ALOGV("Set brightness to %f (signal=%f/value=%d)", *brightness, val,
        normalized);
  std::string brightness_path(sysfs_path_);
  brightness_path += "brightness";
  if (!file_intf_->WriteStringToFile(std::to_string(normalized),
                                     brightness_path)) {
    ALOGE("Failed to set brightness to %d at %s (%d)", normalized,
          brightness_path.c_str(), errno);
    return false;
  }
  return true;
}

}  // namespace android::drm_hwcomposer
