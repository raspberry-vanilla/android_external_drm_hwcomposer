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

#include "bufferinfo/BufferInfoGetter.h"

#include <gtest/gtest.h>

#include <drm/drm_fourcc.h>

namespace android::drm_hwcomposer {

TEST(BufferInfoGetterTest, DrmFormatToBpp32BitFormats) {
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_ARGB8888), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_XRGB8888), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_ABGR8888), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_XBGR8888), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_RGBA8888), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_RGBX8888), 32U);
}

TEST(BufferInfoGetterTest, DrmFormatToBpp10BitFormats) {
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_ARGB2101010), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_XRGB2101010), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_ABGR2101010), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_XBGR2101010), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_RGBA1010102), 32U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_RGBX1010102), 32U);
}

TEST(BufferInfoGetterTest, DrmFormatToBpp16BitFormats) {
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_RGB565), 16U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_BGR565), 16U);
}

TEST(BufferInfoGetterTest, DrmFormatToBppInvalidFormat) {
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(DRM_FORMAT_INVALID), 0U);
  EXPECT_EQ(BufferInfoGetter::DrmFormatToBpp(0xDEADBEEF), 0U);
}

}  // namespace android::drm_hwcomposer
