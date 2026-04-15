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

#include <gtest/gtest.h>
#include <linux/fb.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "utils/BacklightController.h"
#include "utils/BacklightFileInterface.h"
#include "utils/SysfsBacklightController.h"

class SysfsBacklightFileInterfaceFake {
 public:
  SysfsBacklightFileInterfaceFake(const std::string &path,
                                  const std::string &power_val,
                                  const std::string &max_brightness_val,
                                  const std::string &brightness_val,
                                  const std::string &scale_val)
      : path_(std::move(path)),
        power_val_(std::move(power_val)),
        max_brightness_val_(std::move(max_brightness_val)),
        brightness_val_(std::move(brightness_val)),
        scale_val_(std::move(scale_val)) {
  }

  ~SysfsBacklightFileInterfaceFake() = default;

  bool ReadFileToString(const std::string &path, std::string *content,
                        bool /* follow_symlinks */) {
    std::filesystem::path fs_path(path);
    std::filesystem::path file = fs_path.filename().string();
    if (file == "bl_power") {
      *content = power_val_;
    } else if (file == "max_brightness") {
      *content = max_brightness_val_;
    } else if (file == "brightness") {
      *content = brightness_val_;
    } else if (file == "scale") {
      *content = scale_val_;
    } else {
      return false;
    }
    return true;
  }

  bool WriteStringToFile(const std::string &content, const std::string &path,
                         bool /* follow_symlinks */) {
    std::filesystem::path fs_path(path);
    std::filesystem::path file = fs_path.filename().string();
    if (file == "bl_power") {
      power_val_ = content;
    } else if (file == "brightness") {
      brightness_val_ = content;
    } else {
      return false;
    }
    return true;
  }

  std::string path() const {
    return path_;
  }
  std::string power_val() const {
    return power_val_;
  }
  std::string max_brightness_val() const {
    return max_brightness_val_;
  }
  std::string brightness_val() const {
    return brightness_val_;
  }
  std::string scale_val() const {
    return scale_val_;
  }

 private:
  std::string path_;
  std::string power_val_;
  std::string max_brightness_val_;
  std::string brightness_val_;
  std::string scale_val_;
};

std::map<std::string, std::shared_ptr<SysfsBacklightFileInterfaceFake>> kFakes;

namespace android::drm_hwcomposer {

class TestBacklightFileInterface : public BacklightFileInterface {
 public:
  bool ReadFileToString(const std::string &path,
                        std::string *content) override {
    for (auto &[key, fake] : kFakes) {
      if (path.find(key) == 0) {
        return fake->ReadFileToString(path, content, false);
      }
    }
    return false;
  }

  bool WriteStringToFile(const std::string &content,
                         const std::string &path) override {
    for (auto &[key, fake] : kFakes) {
      if (path.find(key) == 0) {
        return fake->WriteStringToFile(content, path, false);
      }
    }
    return false;
  }
};

struct TestSysfsBacklightController {
  TestSysfsBacklightController(const std::string &name, bool powered,
                               int max_brightness, int brightness,
                               bool hw_handles_encoding) {
    std::string path = "/sys/class/backlight/" + name;

    fake_ = std::make_shared<
        SysfsBacklightFileInterfaceFake>(path,
                                         powered
                                             ? std::to_string(FB_BLANK_UNBLANK)
                                             : std::to_string(
                                                   FB_BLANK_POWERDOWN),
                                         std::to_string(max_brightness),
                                         std::to_string(brightness),
                                         hw_handles_encoding ? "linear"
                                                             : "non-linear");
    if (fake_ != nullptr) {
      kFakes[path] = fake_;
    }

    bl_ = SysfsBacklightController::
        CreateInstanceFromName(name,
                               std::make_unique<TestBacklightFileInterface>());
  }

  std::unique_ptr<BacklightController> bl_;
  std::shared_ptr<SysfsBacklightFileInterfaceFake> fake_;
};

class SysfsBacklightControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    kFakes.clear();
  }
};

// Turn on the backlight.
TEST_F(SysfsBacklightControllerTest, TurnOnBacklight) {
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", false,
                                                                 4096, 0, true);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  EXPECT_TRUE(bl.bl_->SetBrightness(1.0F));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_UNBLANK));
  EXPECT_EQ(bl.fake_->brightness_val(), bl.fake_->max_brightness_val());
}

// Turn off the backlight.
TEST_F(SysfsBacklightControllerTest, TurnOffBacklight) {
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 4096, 4096,
                                                                 true);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  EXPECT_TRUE(bl.bl_->SetBrightness(std::nullopt));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_POWERDOWN));
}

// Cycle the backlight on/off/on.
TEST_F(SysfsBacklightControllerTest, CycleBacklight) {
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", false,
                                                                 4096, 0, true);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  // On
  EXPECT_TRUE(bl.bl_->SetBrightness(1.0F));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_UNBLANK));

  // Off
  EXPECT_TRUE(bl.bl_->SetBrightness(std::nullopt));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_POWERDOWN));

  // On
  EXPECT_TRUE(bl.bl_->SetBrightness(1.0F));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_UNBLANK));
}

// Turn on the backlight.
TEST_F(SysfsBacklightControllerTest, TurnOnBacklightWithMinBrightness) {
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", false,
                                                                 4096, 100,
                                                                 true);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  EXPECT_TRUE(bl.bl_->SetBrightness(0.0F));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_UNBLANK));
  EXPECT_EQ(bl.fake_->brightness_val(), "0");
}

// Reduce the brightness to zero, but remain powered.
TEST_F(SysfsBacklightControllerTest, UpdateToZeroBrightness) {
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 4096, 100,
                                                                 true);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  EXPECT_TRUE(bl.bl_->SetBrightness(0.0F));
  EXPECT_EQ(bl.fake_->power_val(), std::to_string(FB_BLANK_UNBLANK));
  EXPECT_EQ(bl.fake_->brightness_val(), "0");
}

// If the hardware handles encoding (scale: linear), the Linear brightness
// values should be passed through.
TEST_F(SysfsBacklightControllerTest, SmartHardware) {
  int max = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 max, 0, true);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  int step_num = 100;
  // Being lazy, make max evenly divisible by step_num to make the loop easy.
  EXPECT_EQ(max % step_num, 0);

  int step_size = max / step_num;
  for (int i = 0; i <= max; i += step_size) {
    EXPECT_TRUE(bl.bl_->SetBrightness(static_cast<float>(i) / max));
    EXPECT_NEAR(std::stoi(bl.fake_->brightness_val()), i, 1);
  }
}

// If the hardware is "passthrough" (scale: non-linear), the Linear brightness
// values should be converted to HLG signal using the OETF.
TEST_F(SysfsBacklightControllerTest, PassthroughHardware) {
  int max = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 max, 0, false);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);

  int step_num = 100;
  // Being lazy, make max evenly divisible by step_num to make the loop easy.
  EXPECT_EQ(max % step_num, 0);

  int step_size = max / step_num;
  for (int i = 0; i <= max; i += step_size) {
    float brightness = static_cast<float>(i) / max;
    EXPECT_TRUE(bl.bl_->SetBrightness(brightness));
    float expected = BacklightController::HlgOetf(brightness) * max;
    EXPECT_NEAR(std::stoi(bl.fake_->brightness_val()),
                static_cast<int>(expected), 1);
  }
}

// Brightness values less than 0 should be rejected.
TEST_F(SysfsBacklightControllerTest, NegativeBrightness) {
  int max = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 max, 0, false);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);
  EXPECT_FALSE(bl.bl_->SetBrightness(-1.0F));
}

// Brightness values greater than 1 should be rejected.
TEST_F(SysfsBacklightControllerTest, OverflowBrightness) {
  int max = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 max, 0, false);
  ASSERT_NE(bl.bl_, nullptr);
  ASSERT_NE(bl.fake_, nullptr);
  EXPECT_FALSE(bl.bl_->SetBrightness(2.0F));
}

}  // namespace android::drm_hwcomposer
