/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "backend/BackendDisplayCapabilities.h"
#include "drm/DrmMode.h"
#include "drm/DrmTestUtils.h"
#include "hwc/HwcDisplayConfigs.h"

namespace android::drm_hwcomposer {

namespace {

DrmMode CreateModeFloat(uint16_t hdisplay, uint16_t vdisplay, float vrefresh,
                        uint32_t flags, uint32_t type, const char* name) {
  drmModeModeInfo mode_info{};
  // We use htotal = 2000, vtotal = 1000 to make math easy.
  mode_info.htotal = 2000;
  mode_info.vtotal = 1000;
  // clock = vrefresh * htotal * vtotal / 1000
  // clock = vrefresh * 2,000,000 / 1000 = vrefresh * 2000
  mode_info.clock = static_cast<uint32_t>(vrefresh * 2000.0F);

  mode_info.hdisplay = hdisplay;
  mode_info.vdisplay = vdisplay;
  mode_info.vrefresh = static_cast<uint32_t>(lround(vrefresh));
  mode_info.flags = flags;
  mode_info.type = type;
  strncpy(mode_info.name, name, DRM_DISPLAY_MODE_LEN - 1);
  return DrmMode(&mode_info);
}

class FakeBackendDisplayCapabilities : public BackendDisplayCapabilities {
 public:
  explicit FakeBackendDisplayCapabilities(std::vector<DrmMode> supported_modes)
      : supported_modes_(std::move(supported_modes)) {
  }

  std::vector<DrmMode> FilterModes(
      const std::vector<DrmMode>& modes) const override {
    std::vector<DrmMode> filtered;
    for (const auto& mode : modes) {
      EXPECT_EQ(mode.GetRawMode().flags & DRM_MODE_FLAG_3D_MASK, 0)
          << "Backend received a 3D mode: " << mode.GetName();
      for (const auto& supported : supported_modes_) {
        if (mode.GetRawMode().hdisplay == supported.GetRawMode().hdisplay &&
            mode.GetRawMode().vdisplay == supported.GetRawMode().vdisplay &&
            std::round(mode.GetVRefresh()) ==
                std::round(supported.GetVRefresh())) {
          filtered.push_back(mode);
          break;
        }
      }
    }
    return filtered;
  }

 private:
  std::vector<DrmMode> supported_modes_;
};

class MockBackendDisplayCapabilities : public BackendDisplayCapabilities {
 public:
  MOCK_METHOD(std::vector<DrmMode>, FilterModes, (const std::vector<DrmMode>&),
              (const, override));
};

}  // namespace

class HwcDisplayConfigsGeneratorTest : public ::testing::Test {
 protected:
  FakeDrmDevice fake_device_;
  HwcDisplayConfigsGenerator generator_;
};

TEST_F(HwcDisplayConfigsGeneratorTest, GetDisplayConfigs_BasicSuccess) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/false,
                                                      /*width=*/500,
                                                      /*height=*/300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1280, 720, 60.0F, 0, 0, "720p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {
      .use_color_pipeline = false,
      .persistent_hdr_enabled = false,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);

  ASSERT_TRUE(configs_opt.has_value());
  const auto& configs = *configs_opt;

  EXPECT_EQ(configs.hwc_configs.size(), 2);
  EXPECT_NE(configs.preferred_config_id, 0);
  EXPECT_EQ(configs.mm_width, 500);
  EXPECT_EQ(configs.mm_height, 300);

  // Check preferred config
  auto preferred_config = configs.hwc_configs.find(configs.preferred_config_id);
  ASSERT_NE(preferred_config, configs.hwc_configs.end());
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().hdisplay, 1920);
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().vdisplay, 1080);
  EXPECT_EQ(preferred_config->second.output_type, OutputType::kSdr);
}

TEST_F(HwcDisplayConfigsGeneratorTest, GetDisplayConfigs_HdrEnabledExternal) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/true, 500,
                                                      300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {
      .use_color_pipeline = true,
      .persistent_hdr_enabled = false,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  EXPECT_EQ(configs_opt->hwc_configs.size(), 2);
  EXPECT_EQ(configs_opt->hwc_configs.begin()->second.output_type,
            OutputType::kSystem);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_HdrDisabledInternalWithoutPersistent) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {
      .use_color_pipeline = true,
      .persistent_hdr_enabled = false,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  EXPECT_EQ(configs_opt->hwc_configs.size(), 1);
  EXPECT_EQ(configs_opt->hwc_configs.begin()->second.output_type,
            OutputType::kSdr);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_HdrEnabledInternalWithPersistent) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {
      .use_color_pipeline = true,
      .persistent_hdr_enabled = true,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  EXPECT_EQ(configs_opt->hwc_configs.size(), 2);
  EXPECT_EQ(configs_opt->hwc_configs.begin()->second.output_type,
            OutputType::kSystem);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_HdrEnabledRegistersDoubleConfigs) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/true, 500,
                                                      300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1280, 720, 60.0F, 0, 0, "720p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {
      .use_color_pipeline = true,
      .persistent_hdr_enabled = false,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  const auto& configs = *configs_opt;

  EXPECT_EQ(configs.hwc_configs.size(), 4);

  int hdr_count = 0;
  int sdr_count = 0;
  for (const auto& [_, config] : configs.hwc_configs) {
    switch (config.output_type) {
      case OutputType::kSystem:
        hdr_count++;
        break;
      case OutputType::kSdr:
        sdr_count++;
        break;
      default:
        FAIL();
    }
  }
  EXPECT_EQ(hdr_count, 2);
  EXPECT_EQ(sdr_count, 2);

  auto preferred_config = configs.hwc_configs.find(configs.preferred_config_id);
  ASSERT_NE(preferred_config, configs.hwc_configs.end());
  EXPECT_EQ(preferred_config->second.output_type, OutputType::kSystem);
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().hdisplay, 1920);
}

TEST_F(HwcDisplayConfigsGeneratorTest, GetDisplayConfigs_Filter3DModes) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1920, 1080, 60.0F, DRM_MODE_FLAG_3D_FRAME_PACKING, 0,
                      "1080p60-3D"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  EXPECT_EQ(configs_opt->hwc_configs.size(), 1);
  EXPECT_EQ(configs_opt->hwc_configs.begin()->second.mode.GetRawMode().hdisplay,
            1920);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_FallbackPreferredMode) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, DRM_MODE_FLAG_3D_FRAME_PACKING,
                      DRM_MODE_TYPE_PREFERRED, "1080p60-3D"),
      CreateModeFloat(1280, 720, 60.0F, 0, 0, "720p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  EXPECT_EQ(configs_opt->hwc_configs.size(), 1);
  EXPECT_NE(configs_opt->preferred_config_id, 0);

  auto preferred_config = configs_opt->hwc_configs.find(
      configs_opt->preferred_config_id);
  ASSERT_NE(preferred_config, configs_opt->hwc_configs.end());
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().hdisplay, 1280);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       SanitizeGroups_EraseTooCloseRefreshRates) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1920, 1080, 60.5F, 0, 0, "1080p60.5"),
      CreateModeFloat(1920, 1080, 90.0F, 0, 0, "1080p90"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  auto configs = std::move(*configs_opt);

  ASSERT_EQ(configs.hwc_configs.size(), 3);

  for (auto& [_, config] : configs.hwc_configs) {
    config.group_id = 1;
  }

  configs.SanitizeGroups();

  EXPECT_EQ(configs.hwc_configs.size(), 2);

  bool has_60 = false;
  bool has_60_5 = false;
  bool has_90 = false;
  for (const auto& [_, config] : configs.hwc_configs) {
    if (config.mode.GetVRefresh() == 60.0F)
      has_60 = true;
    if (config.mode.GetVRefresh() == 60.5F)
      has_60_5 = true;
    if (config.mode.GetVRefresh() == 90.0F)
      has_90 = true;
  }
  EXPECT_TRUE(has_60);
  EXPECT_FALSE(has_60_5);
  EXPECT_TRUE(has_90);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       SanitizeGroups_PreserveDifferentOutputTypes) {
  HwcDisplayConfigs configs;

  configs.hwc_configs[1] = HwcDisplayConfig{
      .id = 1,
      .group_id = 1,
      .mode = CreateModeFloat(1920, 1080, 60.0F, 0, 0, "1080p60"),
      .output_type = OutputType::kSdr,
  };

  configs.hwc_configs[2] = HwcDisplayConfig{
      .id = 2,
      .group_id = 1,
      .mode = CreateModeFloat(1920, 1080, 60.0F, 0, 0, "1080p60"),
      .output_type = OutputType::kSystem,
  };

  configs.SanitizeGroups();

  EXPECT_EQ(configs.hwc_configs.size(), 2);
  ASSERT_TRUE(configs.hwc_configs.count(1));
  EXPECT_EQ(configs.hwc_configs.at(1).output_type, OutputType::kSdr);
  ASSERT_TRUE(configs.hwc_configs.count(2));
  EXPECT_EQ(configs.hwc_configs.at(2).output_type, OutputType::kSystem);
}

TEST_F(HwcDisplayConfigsGeneratorTest, GetDisplayConfigs_NoIdRecycling) {
  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto connector1 = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                       500, 300);
  std::vector<DrmMode> modes1 = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
  };
  connector1->SetModes(modes1);

  auto configs_opt1 = generator_.GenerateDisplayConfigs(*connector1, params);
  ASSERT_TRUE(configs_opt1.has_value());
  ConfigId id1 = configs_opt1->hwc_configs.begin()->first;

  auto connector2 = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                       500, 300);
  std::vector<DrmMode> modes2 = {
      CreateModeFloat(1280, 720, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "720p60"),
  };
  connector2->SetModes(modes2);

  auto configs_opt2 = generator_.GenerateDisplayConfigs(*connector2, params);
  ASSERT_TRUE(configs_opt2.has_value());
  ConfigId id2 = configs_opt2->hwc_configs.begin()->first;

  EXPECT_NE(id1, id2);
  EXPECT_GT(id2, id1);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_EmptyModesReturnsNullopt) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  // Do not call SetModes() -> connector will have an empty modes list.

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  EXPECT_FALSE(configs_opt.has_value());
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_AllModesFilteredOutReturnsNullopt) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  // All provided modes are 3D (unsupported)
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, DRM_MODE_FLAG_3D_FRAME_PACKING, 0,
                      "1080p60-3D"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  EXPECT_FALSE(configs_opt.has_value());
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_PreferredModeNotFirst) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1280, 720, 60.0F, 0, 0, "720p60"),
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1024, 768, 60.0F, 0, 0, "1024x768"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());

  auto preferred_config = configs_opt->hwc_configs.find(
      configs_opt->preferred_config_id);
  ASSERT_NE(preferred_config, configs_opt->hwc_configs.end());
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().hdisplay, 1920);
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().vdisplay, 1080);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_MultiplePreferredModesSelectsFirst) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1, false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1280, 720, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "720p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {.use_color_pipeline = false,
                                .persistent_hdr_enabled = false};

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());

  auto preferred_config = configs_opt->hwc_configs.find(
      configs_opt->preferred_config_id);
  ASSERT_NE(preferred_config, configs_opt->hwc_configs.end());
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().hdisplay, 1920);
  EXPECT_EQ(preferred_config->second.mode.GetRawMode().vdisplay, 1080);
}

TEST_F(HwcDisplayConfigsGeneratorTest, GetFakeMode_VirtualDisplay) {
  auto configs = generator_.GetFakeMode(1920, 1080);

  EXPECT_EQ(configs.hwc_configs.size(), 1);
  EXPECT_NE(configs.preferred_config_id, 0);

  auto config = configs.hwc_configs.find(configs.preferred_config_id);
  ASSERT_NE(config, configs.hwc_configs.end());
  EXPECT_EQ(config->second.mode.GetRawMode().hdisplay, 1920);
  EXPECT_EQ(config->second.mode.GetRawMode().vdisplay, 1080);
  EXPECT_EQ(config->second.mode.GetVRefresh(), 60.0F);
  EXPECT_EQ(config->second.output_type, OutputType::kSystem);
}

TEST_F(HwcDisplayConfigsGeneratorTest, GetFakeMode_HeadlessZeroResolution) {
  // Ensure passing 0x0 during headless boot produces valid default structures
  // without division-by-zero or floating point anomalies.
  auto configs = generator_.GetFakeMode(0, 0);

  EXPECT_EQ(configs.hwc_configs.size(), 1);
  EXPECT_NE(configs.preferred_config_id, 0);
}

TEST_F(HwcDisplayConfigsGeneratorTest,
       GetDisplayConfigs_HdrDisabledWithoutColorPipeline) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/true, 500,
                                                      300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
  };
  connector->SetModes(modes);

  HwcConfigParameters params = {
      .use_color_pipeline = false,
      .persistent_hdr_enabled = true,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  const auto& configs = *configs_opt;

  EXPECT_EQ(configs.hwc_configs.size(), 1);
  EXPECT_EQ(configs.hwc_configs.begin()->second.output_type, OutputType::kSdr);
}

TEST_F(HwcDisplayConfigsGeneratorTest, Init_FilterModesViaCapabilities) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 30.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p30"),
      CreateModeFloat(1920, 1080, 60.0F, 0, 0, "1080p60"),
      CreateModeFloat(1920, 1080, 90.0F, 0, 0, "1080p90"),
  };
  connector->SetModes(modes);

  // Fake capabilities that only support 60Hz
  std::vector<DrmMode> supported_modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, 0, "1080p60"),
  };
  FakeBackendDisplayCapabilities capabilities(supported_modes);

  HwcConfigParameters params = {
      .use_color_pipeline = false,
      .persistent_hdr_enabled = false,
      .capabilities = &capabilities,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
  const auto& configs = *configs_opt;

  // Only 60Hz should remain
  EXPECT_EQ(configs.hwc_configs.size(), 1);
  EXPECT_EQ(configs.hwc_configs.begin()->second.mode.GetVRefresh(), 60.0F);
}

TEST_F(HwcDisplayConfigsGeneratorTest, Init_3DModesFilteredBeforeBackend) {
  auto connector = std::make_unique<FakeDrmConnector>(&fake_device_, 1,
                                                      /*is_external=*/false,
                                                      500, 300);
  std::vector<DrmMode> modes = {
      CreateModeFloat(1920, 1080, 60.0F, 0, DRM_MODE_TYPE_PREFERRED, "1080p60"),
      CreateModeFloat(1920, 1080, 60.0F, DRM_MODE_FLAG_3D_FRAME_PACKING, 0,
                      "1080p60-3D"),
  };
  connector->SetModes(modes);

  MockBackendDisplayCapabilities mock_capabilities;

  // Verify that the backend's FilterModes is called with a list of modes
  // that does NOT contain the 3D mode.
  EXPECT_CALL(mock_capabilities, FilterModes(::testing::_))
      .WillOnce([](const std::vector<DrmMode>& filtered_modes) {
        EXPECT_EQ(filtered_modes.size(), 1);
        EXPECT_EQ(filtered_modes[0].GetRawMode().flags & DRM_MODE_FLAG_3D_MASK,
                  0);
        return filtered_modes;
      });

  HwcConfigParameters params = {
      .use_color_pipeline = false,
      .persistent_hdr_enabled = false,
      .capabilities = &mock_capabilities,
  };

  auto configs_opt = generator_.GenerateDisplayConfigs(*connector, params);
  ASSERT_TRUE(configs_opt.has_value());
}

}  // namespace android::drm_hwcomposer
