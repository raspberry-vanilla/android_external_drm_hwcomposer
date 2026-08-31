/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "BufferInfoGetter.h"

#include <cutils/native_handle.h>
#include <drm/drm_fourcc.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <sys/stat.h>
#include <system/graphics-base-v1.0.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "bufferinfo/GrallocBufferHandle.h"
#include "drm_fourcc.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

// NOLINTNEXTLINE
static std::unique_ptr<BufferInfoGetter> g_buffer_info_getter;

void BufferInfoGetter::Init(std::unique_ptr<BufferInfoGetter> getter) {
  g_buffer_info_getter = std::move(getter);
}

BufferInfoGetter *BufferInfoGetter::GetInstance() {
  return g_buffer_info_getter.get();
}

// NOLINTBEGIN(readability-magic-numbers)
uint32_t BufferInfoGetter::DrmFormatToBpp(uint32_t format) {
  switch (format) {
    case DRM_FORMAT_XRGB16161616:
    case DRM_FORMAT_XBGR16161616:
    case DRM_FORMAT_ARGB16161616:
    case DRM_FORMAT_ABGR16161616:
    case DRM_FORMAT_XRGB16161616F:
    case DRM_FORMAT_XBGR16161616F:
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_ABGR16161616F:
      return 64;
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_RGBX1010102:
      return 32;
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
      return 16;
    default:
      ALOGE("Unsupported format for buffer: 0x%08x, using 32 instead.", format);
      return 32;
  }
}
// NOLINTEND(readability-magic-numbers)

std::shared_ptr<GrallocBufferHandle> BufferInfoGetter::Import(
    buffer_handle_t handle) {
  return GrallocBufferHandle::Create(handle);
}

std::optional<BufferUniqueId> BufferInfoGetter::GetUniqueId(
    buffer_handle_t handle) {
  struct stat sb {};
  if (fstat(handle->data[0], &sb) != 0) {
    return {};
  }

  if (sb.st_size == 0) {
    return {};
  }

  return static_cast<BufferUniqueId>(sb.st_ino);
}

int LegacyBufferInfoGetter::Init() {
  const int ret = hw_get_module(
      GRALLOC_HARDWARE_MODULE_ID,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const hw_module_t **>(&gralloc_));
  if (ret != 0) {
    ALOGE("Failed to open gralloc module");
    return ret;
  }

  ALOGI("Using %s gralloc module: %s\n", gralloc_->common.name,
        gralloc_->common.author);

  return 0;
}

uint32_t LegacyBufferInfoGetter::ConvertHalFormatToDrm(uint32_t hal_format) {
  switch (hal_format) {
    case HAL_PIXEL_FORMAT_RGB_888:
      return DRM_FORMAT_BGR888;
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return DRM_FORMAT_ARGB8888;
    case HAL_PIXEL_FORMAT_RGBX_8888:
      return DRM_FORMAT_XBGR8888;
    case HAL_PIXEL_FORMAT_RGBA_8888:
      return DRM_FORMAT_ABGR8888;
    case HAL_PIXEL_FORMAT_RGB_565:
      return DRM_FORMAT_BGR565;
    case HAL_PIXEL_FORMAT_YV12:
      return DRM_FORMAT_YVU420;
    case HAL_PIXEL_FORMAT_RGBA_1010102:
      return DRM_FORMAT_ABGR2101010;
    case HAL_PIXEL_FORMAT_RGBA_FP16:
      return DRM_FORMAT_ABGR16161616F;
    default:
      ALOGE("Cannot convert hal format to drm format %u", hal_format);
      return DRM_FORMAT_INVALID;
  }
}

bool BufferInfoGetter::IsDrmFormatRgb(uint32_t drm_format) {
  switch (drm_format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_BGR888:
    case DRM_FORMAT_BGR565:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_ABGR16161616F:
      return true;
    default:
      return false;
  }
}

__attribute__((weak)) std::unique_ptr<LegacyBufferInfoGetter>
LegacyBufferInfoGetter::CreateInstance() {
  ALOGE("No legacy buffer info getters available");
  return nullptr;
}

}  // namespace android::drm_hwcomposer