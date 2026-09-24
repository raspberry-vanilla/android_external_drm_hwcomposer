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

#include <SnapHandle.h>
#include <sdm_interface_factory.h>
#include <utils/fence.h>

namespace sdm {

std::shared_ptr<Fence> Fence::Create(int /*fd*/, const char * /*name*/) {
  return nullptr;
}

int Fence::Dup(const std::shared_ptr<Fence> & /*fence*/) {
  return -1;
}

SDMInterfaceFactory *GetSDMInterfaceFactory() {
  return nullptr;
}

}  // namespace sdm

namespace vendor::qti::hardware::display::snapalloc {

SnapHandle *snap_handle_create(int /*numFds*/, int /*numInts*/) {
  return nullptr;
}

}  // namespace vendor::qti::hardware::display::snapalloc
