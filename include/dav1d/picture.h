/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DAV1D_PICTURE_H__
#define __DAV1D_PICTURE_H__

#include <stddef.h>
#include <stdint.h>

#include "common.h"

enum Dav1dPixelLayout {
    DAV1D_PIXEL_LAYOUT_I400, ///< monochrome
    DAV1D_PIXEL_LAYOUT_I420, ///< 4:2:0 planar
    DAV1D_PIXEL_LAYOUT_I422, ///< 4:2:2 planar
    DAV1D_PIXEL_LAYOUT_I444, ///< 4:4:4 planar
};

enum Dav1dFrameType {
    DAV1D_FRAME_TYPE_KEY = 0,    ///< Key Intra frame
    DAV1D_FRAME_TYPE_INTER = 1,  ///< Inter frame
    DAV1D_FRAME_TYPE_INTRA = 2,  ///< Non key Intra frame
    DAV1D_FRAME_TYPE_SWITCH = 3, ///< Switch Inter frame
};

enum Dav1dColorPrimaries {
    DAV1D_COLOR_PRI_BT709 = 1,
    DAV1D_COLOR_PRI_UNKNOWN = 2,
    DAV1D_COLOR_PRI_BT470M = 4,
    DAV1D_COLOR_PRI_BT470BG = 5,
    DAV1D_COLOR_PRI_BT601 = 6,
    DAV1D_COLOR_PRI_SMPTE240 = 7,
    DAV1D_COLOR_PRI_FILM = 8,
    DAV1D_COLOR_PRI_BT2020 = 9,
    DAV1D_COLOR_PRI_XYZ = 10,
    DAV1D_COLOR_PRI_SMPTE431 = 11,
    DAV1D_COLOR_PRI_SMPTE432 = 12,
    DAV1D_COLOR_PRI_EBU3213 = 22,
};

enum Dav1dTransferCharacteristics {
    DAV1D_TRC_BT709 = 1,
    DAV1D_TRC_UNKNOWN = 2,
    DAV1D_TRC_BT470M = 4,
    DAV1D_TRC_BT470BG = 5,
    DAV1D_TRC_BT601 = 6,
    DAV1D_TRC_SMPTE240 = 7,
    DAV1D_TRC_LINEAR = 8,
    DAV1D_TRC_LOG100 = 9,         ///< logarithmic (100:1 range)
    DAV1D_TRC_LOG100_SQRT10 = 10, ///< lograithmic (100*sqrt(10):1 range)
    DAV1D_TRC_IEC61966 = 11,
    DAV1D_TRC_BT1361 = 12,
    DAV1D_TRC_SRGB = 13,
    DAV1D_TRC_BT2020_10BIT = 14,
    DAV1D_TRC_BT2020_12BIT = 15,
    DAV1D_TRC_SMPTE2084 = 16,     ///< PQ
    DAV1D_TRC_SMPTE428 = 17,
    DAV1D_TRC_HLG = 18,           ///< hybrid log/gamma (BT.2100 / ARIB STD-B67)
};

enum Dav1dMatrixCoefficients {
    DAV1D_MC_IDENTITY = 0,
    DAV1D_MC_BT709 = 1,
    DAV1D_MC_UNKNOWN = 2,
    DAV1D_MC_FCC = 4,
    DAV1D_MC_BT470BG = 5,
    DAV1D_MC_BT601 = 6,
    DAV1D_MC_SMPTE240 = 7,
    DAV1D_MC_SMPTE_YCGCO = 8,
    DAV1D_MC_BT2020_NCL = 9,
    DAV1D_MC_BT2020_CL = 10,
    DAV1D_MC_SMPTE2085 = 11,
    DAV1D_MC_CHROMAT_NCL = 12, ///< Chromaticity-derived
    DAV1D_MC_CHROMAT_CL = 13,
    DAV1D_MC_ICTCP = 14,
};

enum Dav1dChromaSamplePosition {
    DAV1D_CHR_UNKNOWN = 0,
    DAV1D_CHR_VERTICAL = 1,  ///< Horizontally co-located with luma(0, 0)
                           ///< sample, between two vertical samples
    DAV1D_CHR_COLOCATED = 2, ///< Co-located with luma(0, 0) sample
};

typedef struct Dav1dPictureParameters {
    int w; ///< width (in pixels)
    int h; ///< height (in pixels)
    enum Dav1dPixelLayout layout; ///< format of the picture
    enum Dav1dFrameType type; ///< type of the picture
    int bpc; ///< bits per pixel component (8 or 10)

    enum Dav1dColorPrimaries pri; ///< color primaries (av1)
    enum Dav1dTransferCharacteristics trc; ///< transfer characteristics (av1)
    enum Dav1dMatrixCoefficients mtrx; ///< matrix coefficients (av1)
    enum Dav1dChromaSamplePosition chr; ///< chroma sample position (av1)
    /**
     * Pixel data uses JPEG pixel range ([0,255] for 8bits) instead of
     * MPEG pixel range ([16,235] for 8bits luma, [16,240] for 8bits chroma).
     */
    int fullrange;
} Dav1dPictureParameters;

typedef struct Dav1dPicture {
    /**
     * Pointers to planar image data (Y is [0], U is [1], V is [2]). The data
     * should be bytes (for 8 bpc) or words (for 10 bpc). In case of words
     * containing 10 bpc image data, the pixels should be located in the LSB
     * bits, so that values range between [0, 1023]; the upper bits should be
     * zero'ed out.
     */
    void *data[3];
    struct Dav1dRef *ref; ///< allocation origin

    /**
     * Number of bytes between 2 lines in data[] for luma [0] or chroma [1].
     */
    ptrdiff_t stride[2];

    Dav1dPictureParameters p;

    int poc; ///< frame number
} Dav1dPicture;

/**
 * Release reference to a picture.
 */
DAV1D_API void dav1d_picture_unref(Dav1dPicture *p);

#endif /* __DAV1D_PICTURE_H__ */
