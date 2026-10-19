// Licence selection for this modified copy (2026-10-19): under LGPL section 3,
// references to the LGPL in the retained upstream notice below are replaced
// by the GNU General Public License, version 2, for the adapted LGPL portion.
// The upstream notice is reproduced verbatim as provenance of the original.
// Modified for the Ghoulies native Windows preview, 2026-10-14 through 2026-10-19.
// Port integration, validation and rendering changes: see docs/PORT-PROVENANCE.md.
/*
 * QEMU texture swizzling routines
 *
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2013 espes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#ifndef HW_XBOX_SWIZZLE_H
#define HW_XBOX_SWIZZLE_H

#include "compat.h"
#include <stdint.h>

void swizzle_box(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    unsigned int depth,
    uint8_t *dst_buf,
    unsigned int row_pitch,
    unsigned int slice_pitch,
    unsigned int bytes_per_pixel);

void unswizzle_box(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    unsigned int depth,
    uint8_t *dst_buf,
    unsigned int row_pitch,
    unsigned int slice_pitch,
    unsigned int bytes_per_pixel);

void unswizzle_rect(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    uint8_t *dst_buf,
    unsigned int pitch,
    unsigned int bytes_per_pixel);

void swizzle_rect(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    uint8_t *dst_buf,
    unsigned int pitch,
    unsigned int bytes_per_pixel);

#endif
