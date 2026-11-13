// SPDX-License-Identifier: Apache-2.0
//
// veux libqcodec2_utils.so sizes HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS with the
// msm_media_info generation it was built with: 512-byte stride and 512-line
// scanlines. Gralloc and the legacy video driver here use 128-byte stride,
// 32-line luma and 16-line chroma scanlines, so the C2D converter rejects
// every gralloc NV12 buffer. Give that format the gralloc layout in
// QC2C2DEngine::calcSize and calcYSize; NV12_512 and HEIF keep 512.

.text
// calcSize: w20 = width, w19 = height, w23 = format, result in w21.
.org 0x3a418
    b calc_size
calc_size_return:
.org 0x3a6bc
calc_size_done:
// calcYSize: w2 = width, w3 = height, w19 = format.
.org 0x3a724
    b calc_ysize
calc_ysize_return:
.org 0x3a81c
calc_ysize_mul:

.org 0x449e0
calc_size:
    movz w8, #0x0c04
    movk w8, #0x7fa3, lsl #16
    cmp w23, w8
    b.ne 1f
    add w8, w20, #0x7f
    and w8, w8, #0xffffff80
    add w9, w19, #0x1f
    and w9, w9, #0xffffffe0
    add w10, w19, #1
    lsr w10, w10, #1
    add w10, w10, #0xf
    and w10, w10, #0xfffffff0
    add w9, w9, w10
    mul w8, w8, w9
    add w8, w8, #0xfff
    and w21, w8, #0xfffff000
    b calc_size_done
1:
    movz w8, #0xf3fc
    b calc_size_return

.p2align 4
calc_ysize:
    movz w8, #0x0c04
    movk w8, #0x7fa3, lsl #16
    cmp w19, w8
    b.ne 1f
    add w8, w2, #0x7f
    and w8, w8, #0xffffff80
    add w9, w3, #0x1f
    and w9, w9, #0xffffffe0
    b calc_ysize_mul
1:
    movz w8, #0xf3fc
    b calc_ysize_return
