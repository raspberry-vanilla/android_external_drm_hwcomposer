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

#include <ISnapMapper.h>

// Note: The real hwc_buffer_allocator.h omits #include
// <core/buffer_allocator.h> and relies on callers including it (or
// sdm_display_intf_layer_builder.h) first.
namespace sdm {

class HWCBufferAllocator : public BufferAllocator {
 public:
  HWCBufferAllocator() = default;
  ~HWCBufferAllocator() override = default;
};

}  // namespace sdm
