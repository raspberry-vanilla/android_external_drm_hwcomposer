/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <cmath>
#include <cstdbool>
#include <cstdint>
#include <optional>
#include <vector>

#include "bufferinfo/BufferInfo.h"
#include "compositor/DisplayInfo.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

class IDrmFbIdHandle;

using ILayerId = int64_t;

enum class CompositionType {
  kInvalid,
  kClient,
  kDevice,
  kSolidColor,
  kCursor,
  kDeviceOccluded
};

enum class TransferFunction : int32_t {
  kUnknown,
  kSrgb,
  kPq,
};

/* Rotation is defined in the clockwise direction */
/* The flip is done before rotation */
struct LayerTransform {
  bool hflip;
  bool vflip;
  bool rotate90;
};

template <typename T>
struct Rect {
  T left;
  T top;
  T right;
  T bottom;

  T Width() const {
    return right - left;
  }

  T Height() const {
    return bottom - top;
  }
};

using IRect = Rect<int32_t>;
using FRect = Rect<float>;

struct SrcRectInfo {
  /* nullopt means the whole buffer */
  std::optional<FRect> f_rect;
};

struct DstRectInfo {
  /* nullopt means the whole display */
  std::optional<IRect> i_rect;
};

struct DamageInfo {
  /* Empty vector means the whole source buffer may have been modified. */
  std::vector<IRect> dmg_rects;
};

constexpr float kAlphaOpaque = 1.0F;

struct PresentInfo {
  LayerTransform transform{};
  float alpha = kAlphaOpaque;
  SrcRectInfo source_crop{};
  DstRectInfo display_frame{};
  DamageInfo damage{};

  bool RequireScalingOrPhasing() const {
    if (!source_crop.f_rect || !display_frame.i_rect) {
      return false;
    }

    const auto &src = *source_crop.f_rect;
    const auto &dst = *display_frame.i_rect;

    auto scaling = src.Width() != static_cast<float>(dst.Width()) ||
                   src.Height() != static_cast<float>(dst.Height());
    auto phasing = (src.left - std::floor(src.left) != 0) ||
                   (src.top - std::floor(src.top) != 0);
    return scaling || phasing;
  }
};

struct LayerData {
  std::optional<BufferInfo> bi;
  std::shared_ptr<IDrmFbIdHandle> fb;
  PresentInfo pi;
  SharedFd acquire_fence;
  Colorspace colorspace;
  TransferFunction transfer_func;
};

}  // namespace android::drm_hwcomposer
