/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef HWC_DISPLAY_BACKEND_RPI5_H
#define HWC_DISPLAY_BACKEND_RPI5_H

#include "Backend.h"

namespace android {

class BackendRpi5 : public Backend {
 public:
  bool IsClientLayer(HwcDisplay *display, HwcLayer *layer) override;
};
}  // namespace android

#endif
