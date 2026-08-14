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
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "utils/BacklightController.h"
#include "utils/BacklightFileInterface.h"
#include "utils/ColorUtil.h"
#include "utils/SysfsBacklightController.h"
#include "utils/TestUtils.h"

namespace android::drm_hwcomposer {

class SysfsBacklightFileInterfaceFake {
 public:
  SysfsBacklightFileInterfaceFake(std::string path, std::string power_val,
                                  std::string max_brightness_val,
                                  std::string brightness_val,
                                  std::string scale_val)
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

  std::string Path() const {
    return path_;
  }
  std::string PowerVal() const {
    return power_val_;
  }
  std::string MaxBrightnessVal() const {
    return max_brightness_val_;
  }
  std::string BrightnessVal() const {
    return brightness_val_;
  }
  std::string ScaleVal() const {
    return scale_val_;
  }

 private:
  std::string path_;
  std::string power_val_;
  std::string max_brightness_val_;
  std::string brightness_val_;
  std::string scale_val_;
};

class TestBacklightFileInterface : public BacklightFileInterface {
 public:
  explicit TestBacklightFileInterface(
      std::shared_ptr<SysfsBacklightFileInterfaceFake> fake)
      : fake_(std::move(fake)) {
  }

  bool ReadFileToString(const std::string &path,
                        std::string *content) override {
    if (fake_) {
      return fake_->ReadFileToString(path, content, false);
    }
    return false;
  }

  bool WriteStringToFile(const std::string &content,
                         const std::string &path) override {
    if (fake_) {
      return fake_->WriteStringToFile(content, path, false);
    }
    return false;
  }

 private:
  std::shared_ptr<SysfsBacklightFileInterfaceFake> fake_;
};

struct TestSysfsBacklightController {
  TestSysfsBacklightController(const std::string &name, bool powered,
                               int max_brightness, int brightness,
                               bool hw_handles_encoding) {
    std::string path = "/sys/class/backlight/" + name;

    fake = std::make_shared<
        SysfsBacklightFileInterfaceFake>(path,
                                         powered
                                             ? std::to_string(FB_BLANK_UNBLANK)
                                             : std::to_string(
                                                   FB_BLANK_POWERDOWN),
                                         std::to_string(max_brightness),
                                         std::to_string(brightness),
                                         hw_handles_encoding ? "linear"
                                                             : "non-linear");

    bl = SysfsBacklightController::
        CreateInstanceFromName(name,
                               std::make_unique<TestBacklightFileInterface>(
                                   fake));
  }

  std::unique_ptr<BacklightController> bl;
  std::shared_ptr<SysfsBacklightFileInterfaceFake> fake;
};

class SysfsBacklightControllerTest : public ::testing::Test {};

// Turn on the backlight.
TEST_F(SysfsBacklightControllerTest, TurnOnBacklight) {
  static constexpr int kMaxBrightness = 4096;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", false,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  EXPECT_TRUE(bl.bl->SetBrightness(1.0F));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_UNBLANK));
  EXPECT_EQ(bl.fake->BrightnessVal(), bl.fake->MaxBrightnessVal());
}

// Turn off the backlight.
TEST_F(SysfsBacklightControllerTest, TurnOffBacklight) {
  static constexpr int kMaxBrightness = 4096;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 kMaxBrightness,
                                                                 true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  EXPECT_TRUE(bl.bl->SetBrightness(std::nullopt));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_POWERDOWN));
}

// Cycle the backlight on/off/on.
TEST_F(SysfsBacklightControllerTest, CycleBacklight) {
  static constexpr int kMaxBrightness = 4096;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", false,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  // On
  EXPECT_TRUE(bl.bl->SetBrightness(1.0F));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_UNBLANK));

  // Off
  EXPECT_TRUE(bl.bl->SetBrightness(std::nullopt));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_POWERDOWN));

  // On
  EXPECT_TRUE(bl.bl->SetBrightness(1.0F));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_UNBLANK));
}

// Turn on the backlight.
TEST_F(SysfsBacklightControllerTest, TurnOnBacklightWithMinBrightness) {
  static constexpr int kMaxBrightness = 4096;
  static constexpr int kBrightness = 100;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", false,
                                                                 kMaxBrightness,
                                                                 kBrightness,
                                                                 true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  EXPECT_TRUE(bl.bl->SetBrightness(0.0F));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_UNBLANK));
  EXPECT_EQ(bl.fake->BrightnessVal(), "1");
}

// Reduce the brightness to zero, but remain powered.
TEST_F(SysfsBacklightControllerTest, UpdateToZeroBrightness) {
  static constexpr int kMaxBrightness = 4096;
  static constexpr int kBrightness = 100;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 kBrightness,
                                                                 true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  EXPECT_TRUE(bl.bl->SetBrightness(0.0F));
  EXPECT_EQ(bl.fake->PowerVal(), std::to_string(FB_BLANK_UNBLANK));
  EXPECT_EQ(bl.fake->BrightnessVal(), "1");
}

// If the hardware handles encoding (scale: linear), the Linear brightness
// values should be passed through.
TEST_F(SysfsBacklightControllerTest, SmartHardware) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  static constexpr int kStepNum = 100;
  // Being lazy, make kMaxBrightness evenly divisible by kStepNum to make the
  // loop easy.
  EXPECT_EQ(kMaxBrightness % kStepNum, 0);

  int step_size = kMaxBrightness / kStepNum;
  for (int i = 0; i <= kMaxBrightness; i += step_size) {
    EXPECT_TRUE(bl.bl->SetBrightness(static_cast<float>(i) / kMaxBrightness));
    EXPECT_NEAR(std::stoi(bl.fake->BrightnessVal()), i, 1);
  }
}

// If the hardware is "passthrough" (scale: non-linear), the Linear brightness
// values should be converted to HLG signal using the OETF.
TEST_F(SysfsBacklightControllerTest, PassthroughHardware) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, false);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  static constexpr int kStepNum = 100;
  // Being lazy, make kMaxBrightness evenly divisible by kStepNum to make the
  // loop easy.
  EXPECT_EQ(kMaxBrightness % kStepNum, 0);

  int step_size = kMaxBrightness / kStepNum;
  for (int i = 0; i <= kMaxBrightness; i += step_size) {
    float brightness = static_cast<float>(i) / kMaxBrightness;
    EXPECT_TRUE(bl.bl->SetBrightness(brightness));
    float expected = BacklightController::HlgOetf(brightness) * kMaxBrightness;
    EXPECT_NEAR(std::stoi(bl.fake->BrightnessVal()), static_cast<int>(expected),
                1);
  }
}

// Brightness values less than 0 should be rejected.
TEST_F(SysfsBacklightControllerTest, NegativeBrightness) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, false);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);
  EXPECT_FALSE(bl.bl->SetBrightness(-1.0F));
}

// Brightness values greater than 1 should be rejected.
TEST_F(SysfsBacklightControllerTest, OverflowBrightness) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, false);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);
  EXPECT_FALSE(bl.bl->SetBrightness(2.0F));
}

TEST_F(SysfsBacklightControllerTest,
       SetBrightnessWithMinDisplayBrightnessClamping) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  ScopedTestProperty prop("vendor.hwc.drm.min_display_brightness", "0.1");
  EXPECT_TRUE(bl.bl->SetBrightness(ColorUtil::ScaleBrightnessIfNeeded(0.0F)));
  // 0.0F scaled to 0.1F -> 1 + (10000 - 1) * 0.1 = 1000
  EXPECT_EQ(bl.fake->BrightnessVal(), "1000");
}

TEST_F(SysfsBacklightControllerTest,
       SetBrightnessWithScaledBrightnessRangeNoopWhenMinZero) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  EXPECT_TRUE(bl.bl->SetBrightness(ColorUtil::ScaleBrightnessIfNeeded(0.0F)));
  EXPECT_EQ(bl.fake->BrightnessVal(), "1");
}

TEST_F(SysfsBacklightControllerTest,
       SetBrightnessWithScaledBrightnessRangeNonZeroAtZero) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  EXPECT_TRUE(bl.bl->SetBrightness(ColorUtil::ScaleBrightnessIfNeeded(0.0F)));
  // 0.0F scaled to 0.1F -> 1 + (10000 - 1) * 0.1 = 1000
  EXPECT_EQ(bl.fake->BrightnessVal(), "1000");
}

TEST_F(SysfsBacklightControllerTest,
       SetBrightnessWithScaledBrightnessRangeCalculatesExpectedScale) {
  static constexpr int kMaxBrightness = 10000;
  TestSysfsBacklightController bl = TestSysfsBacklightController("bl-0", true,
                                                                 kMaxBrightness,
                                                                 0, true);
  ASSERT_NE(bl.bl, nullptr);
  ASSERT_NE(bl.fake, nullptr);

  ScopedTestProperty min_prop("vendor.hwc.drm.min_display_brightness", "0.1");
  ScopedTestProperty
      scale_prop("vendor.hwc.drm.scale_brightness_range_to_min_brightness",
                 "true");

  EXPECT_TRUE(bl.bl->SetBrightness(ColorUtil::ScaleBrightnessIfNeeded(0.5F)));
  // 0.5F scaled to 0.55F -> 1 + (10000 - 1) * 0.55 = 5500
  EXPECT_EQ(bl.fake->BrightnessVal(), "5500");
}

}  // namespace android::drm_hwcomposer
