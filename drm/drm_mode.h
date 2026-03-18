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

#include <drm_mode.h>
#include <linux/types.h>

#ifndef DRM_MODE_OBJECT_COLOROP
#define DRM_MODE_OBJECT_COLOROP 0xfafafafa

struct drm_color_ctm_3x4 {
	/*
	 * Conversion matrix with 3x4 dimensions in S31.32 sign-magnitude
	 * (not two's complement!) format.
	 *
	 * out   matrix          in
	 * |R|   |0  1  2  3 |   | R |
	 * |G| = |4  5  6  7 | x | G |
	 * |B|   |8  9  10 11|   | B |
	 *                       |1.0|
	 */
	__u64 matrix[12];
};

struct drm_color_lut32 {
  /*
   * Similar to drm_color_lut but for high precision LUTs
   */
  __u32 red;
  __u32 green;
  __u32 blue;
  __u32 reserved;
};

/**
 * enum drm_colorop_type - Type of color operation
 *
 * drm_colorops can be of many different types. Each type behaves differently
 * and defines a different set of properties. This enum defines all types and
 * gives a high-level description.
 */

enum drm_colorop_type {
  /**
   * @DRM_COLOROP_1D_CURVE:
   *
   * enum string "1D Curve"
   *
   * A 1D curve that is being applied to all color channels. The
   * curve is specified via the CURVE_1D_TYPE colorop property.
   */
  DRM_COLOROP_1D_CURVE,

  /**
   * @DRM_COLOROP_1D_LUT:
   *
   * enum string "1D LUT"
   *
   * A simple 1D LUT of uniformly spaced &drm_color_lut32 entries,
   * packed into a blob via the DATA property. The driver's
   * expected LUT size is advertised via the SIZE property.
   *
   * The DATA blob is an array of struct drm_color_lut32 with size
   * of "lut_size".
   */
  DRM_COLOROP_1D_LUT,

  /**
   * @DRM_COLOROP_CTM_3X4:
   *
   * enum string "3x4 Matrix"
   *
   * A 3x4 matrix. Its values are specified via the
   * &drm_color_ctm_3x4 struct provided via the DATA property.
   *
   * The DATA blob is a float[12]:
   * out   matrix          in
   * | R |   | 0  1  2  3  |   | R |
   * | G | = | 4  5  6  7  | x | G |
   * | B |   | 8  9  10 12 |   | B |
   */
  DRM_COLOROP_CTM_3X4,

  /**
   * @DRM_COLOROP_MULTIPLIER:
   *
   * enum string "Multiplier"
   *
   * A simple multiplier, applied to all color values. The
   * multiplier is specified as a S31.32 via the MULTIPLIER
   * property.
   */
  DRM_COLOROP_MULTIPLIER,

  /**
   * @DRM_COLOROP_3D_LUT:
   *
   * enum string "3D LUT"
   *
   * A 3D LUT of &drm_color_lut32 entries,
   * packed into a blob via the DATA property. The driver's expected
   * LUT size is advertised via the SIZE property, i.e., a 3D LUT with
   * 17x17x17 entries will have SIZE set to 17.
   *
   * The DATA blob is a 3D array of struct drm_color_lut32 with dimension
   * length of "lut_size".
   * The LUT elements are traversed like so:
   *
   *   for B in range 0..n
   *     for G in range 0..n
   *       for R in range 0..n
   *        index = R + n * (G + n * B)
   *         color = lut3d[index]
   */
  DRM_COLOROP_3D_LUT,
};

/**
 * enum drm_colorop_lut3d_interpolation_type - type of 3DLUT interpolation
 */
enum drm_colorop_lut3d_interpolation_type {
	/**
	 * @DRM_COLOROP_LUT3D_INTERPOLATION_TETRAHEDRAL:
	 *
	 * Tetrahedral 3DLUT interpolation
	 */
	DRM_COLOROP_LUT3D_INTERPOLATION_TETRAHEDRAL,
};

/**
 * enum drm_colorop_lut1d_interpolation_type - type of interpolation for 1D LUTs
 */
enum drm_colorop_lut1d_interpolation_type {
	/**
	 * @DRM_COLOROP_LUT1D_INTERPOLATION_LINEAR:
	 *
	 * Linear interpolation. Values between points of the LUT will be
	 * linearly interpolated.
	 */
	DRM_COLOROP_LUT1D_INTERPOLATION_LINEAR,
};

#endif
// NOLINTEND
