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

#pragma once

#include <cstdint>

#include <core/layer_stack.h>
#include <core/sdm_types.h>

namespace sdm {

struct DisplayConfigVariableInfo {
  uint32_t x_pixels = 0;
  uint32_t y_pixels = 0;
  uint32_t fps = 0;
  uint32_t vsync_period_ns = 0;
};

}  // namespace sdm
