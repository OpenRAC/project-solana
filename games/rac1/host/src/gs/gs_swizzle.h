/* gs_swizzle.h: where a pixel lives in GS local memory (4 MB) for each pixel
 * storage format. Memory is pages of 8 KB (32 blocks of 256 bytes); inside a
 * block, pixels are arranged in 4 columns. The layouts below are the ones the
 * GS hardware uses (GS User's Manual, section 8). */
#ifndef GS_SWIZZLE_H
#define GS_SWIZZLE_H
#include <stdint.h>

static const uint8_t gs_block32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t gs_block16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t gs_block16s[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
/* 8-bit uses the 32-bit block order, 4-bit the 16-bit one */
#define gs_block8 gs_block32
#define gs_block4 gs_block16

static uint8_t gs_col32[8][8];     /* word in block */
static uint8_t gs_col16[8][16];    /* halfword in block */
static uint8_t gs_col8[16][16];    /* byte in block */
static uint16_t gs_col4[16][32];   /* nibble in block */

static void gs_swizzle_init(void)
{
    /* 32-bit: a column is 8x2 pixels; pairs of words alternate between rows */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            gs_col32[y][x] = (uint8_t)((y >> 1) * 16 + (x >> 1) * 4 + (y & 1) * 2 + (x & 1));
    /* 16-bit: two 16-bit pixels share a 32-bit word; x bit 3 picks the half */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++)
            gs_col16[y][x] = (uint8_t)(gs_col32[y][x & 7] * 2 + ((x >> 3) & 1));
    /* 8-bit and 4-bit: each column (4 rows) is laid out like a 32-bit column
     * in which odd columns swap their halves */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int col = y >> 2, r = y & 3;
            int half = ((r >> 1) ^ (col & 1)) & 1;        /* which half of the column's words */
            int grp = x >> 3, xi = x & 7;
            int base[8] = {0, 4, 16, 20, 32, 36, 48, 52};
            int b = base[(xi + (half ? 4 : 0)) & 7];
            if (r >> 1) b += 1;                           /* rows 2 and 3 hold the odd bytes */
            gs_col8[y][x] = (uint8_t)(col * 64 + b + (r & 1) * 8 + grp * 2);
        }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 32; x++) {
            int col = y >> 2, r = y & 3;
            int half = ((r >> 1) ^ (col & 1)) & 1;
            int grp = x >> 3, xi = x & 7;
            int base[8] = {0, 8, 32, 40, 64, 72, 96, 104};
            int b = base[(xi + (half ? 4 : 0)) & 7];
            if (r >> 1) b += 1;
            gs_col4[y][x] = (uint16_t)(col * 128 + b + (r & 1) * 16 + grp * 2);
        }
}
#endif
