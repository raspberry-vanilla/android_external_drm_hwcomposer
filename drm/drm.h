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
#pragma once
// NOLINTBEGIN

#include <drm.h>  // IWYU pragma: export

/**
 * DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE
 *
 * If set to 1 the DRM core will allow setting the COLOR_PIPELINE
 * property on a &drm_plane, as well as drm_colorop properties.
 *
 * Setting of these plane properties will be rejected when this client
 * cap is set:
 * - COLOR_ENCODING
 * - COLOR_RANGE
 *
 * The client must enable &DRM_CLIENT_CAP_ATOMIC first.
 */
#ifndef DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE
#define DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE 7
#endif
// NOLINTEND
