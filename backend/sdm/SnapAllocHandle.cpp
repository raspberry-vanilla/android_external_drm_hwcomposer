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

#include "backend/sdm/SnapAllocHandle.h"

#include <dlfcn.h>
#include <fcntl.h>  // NOLINT(misc-include-cleaner)

#include <functional>
#include <memory>
#include <string>

#include <cutils/native_handle.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Errors.h>

#include <Error.h>
#include <ISnapMapper.h>
#include <SnapHandle.h>

#include "bufferinfo/GrallocBufferHandle.h"
#include "utils/log.h"

using ISnapMapper = vendor::qti::hardware::display::snapalloc::ISnapMapper;
using SnapError = vendor::qti::hardware::display::snapalloc::Error;
using SnapHandle = vendor::qti::hardware::display::snapalloc::SnapHandle;
using vendor::qti::hardware::display::snapalloc::snap_handle_create;

namespace android::drm_hwcomposer {
namespace {
// TODO: This was copied from hwc_common; expose this from hwc_common rather
// than re-defining here.
SnapHandle* ConvertToSnapHandle(const buffer_handle_t handle) {
  SnapHandle* snap_handle = snap_handle_create(handle->numFds, handle->numInts);
  if (snap_handle != nullptr) {
    for (int i = 0; i < handle->numFds; ++i) {
      // NOLINTNEXTLINE(misc-include-cleaner)
      snap_handle->buffer_data[i] = fcntl(handle->data[i], F_DUPFD_CLOEXEC, 0);
    }
    for (int i = 0; i < handle->numInts; ++i) {
      snap_handle
          ->buffer_data[i + handle->numFds] = handle->data[handle->numFds + i];
    }
  }
  return snap_handle;
}

// TODO: This was copied from qti composer; expose this from some library rather
// than re-defining here.
using SnapMapperFactoryFcn = std::function<std::shared_ptr<ISnapMapper>(
    sdm::DebugCallbackIntf*)>;
SnapMapperFactoryFcn GetSnapMapperFcn() {
  const std::string
      snapalloc_lib_name = "vendor.qti.hardware.display.snapalloc-impl.so";
  void* snap_impl_lib = ::dlopen(snapalloc_lib_name.c_str(), RTLD_NOW);
  if (snap_impl_lib == nullptr) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    ALOGE("Dlopen error for snapalloc impl: %s", dlerror());
  }

  std::shared_ptr<ISnapMapper> (*link_fetch_isnap_mapper)(
      sdm::DebugCallbackIntf*) = nullptr;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  *reinterpret_cast<void**>(
      &link_fetch_isnap_mapper) = ::dlsym(snap_impl_lib, "FETCH_ISnapMapper");

  ALOGE_IF(link_fetch_isnap_mapper == nullptr,
           "Failed to get snapalloc instances.");
  return link_fetch_isnap_mapper;
}

// TODO: Confirm that this is threadsafe.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::shared_ptr<ISnapMapper> g_snap_mapper;

}  // namespace

bool SnapAllocHandle::Init(sdm::DebugCallbackIntf* debug_callback_intf) {
  ALOGI("Init SnapMapper");
  auto snap_mapper_fcn = GetSnapMapperFcn();
  if (snap_mapper_fcn != nullptr) {
    g_snap_mapper = snap_mapper_fcn(debug_callback_intf);
  }
  return g_snap_mapper != nullptr;
}

std::shared_ptr<SnapAllocHandle> SnapAllocHandle::Create(
    buffer_handle_t handle) {
  // import into process
  buffer_handle_t imported_handle = {};
  auto result = android::GraphicBufferMapper::get()
                    .importBufferNoValidate(handle, &imported_handle);
  if (result != android::NO_ERROR) {
    ALOGE("Failed to import buffer handle: %d", result);
    return nullptr;
  }
  // convert to snap handle
  SnapHandle* snap_handle = ConvertToSnapHandle(imported_handle);
  if (snap_handle == nullptr) {
    ALOGE("Failed to convert imported handle to SnapHandle");
    return nullptr;
  }
  SnapError error = g_snap_mapper->Retain(*snap_handle);
  if (error != SnapError::NONE) {
    ALOGE("Failed to retain SnapHandle: %d", error);
    return nullptr;
  }
  // SnapAllocHandle c'tor is private so can't use std::make_shared.
  return std::shared_ptr<SnapAllocHandle>(
      new SnapAllocHandle(imported_handle, snap_handle));
}

SnapAllocHandle::SnapAllocHandle(buffer_handle_t buffer_handle,
                                 SnapHandle* snap_handle)
    : GrallocBufferHandle(buffer_handle), snap_handle_(snap_handle) {
}

SnapHandle* SnapAllocHandle::GetSnapHandle() const {
  return snap_handle_;
}

SnapAllocHandle::~SnapAllocHandle() {
  SnapError error = g_snap_mapper->Release(*snap_handle_);
  ALOGE_IF(error != SnapError::NONE, "Failed to release SnapHandle: %d", error);
}

}  // namespace android::drm_hwcomposer