/* pss.h: MPEG-2 program stream (PS2 .pss) demuxer: pulls the video elementary
 * stream (stream id 0xE0) out of pack/PES framing. */
#ifndef UYA_PSS_H
#define UYA_PSS_H
#include <stdint.h>
#include <stddef.h>

/* Appends the payload of every video PES packet in `ps` to `out` (capacity
 * `cap`); returns the number of bytes written. */
static size_t pss_extract_video(const uint8_t *ps, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, w = 0;
    while (i + 4 <= n) {
        if (ps[i] != 0 || ps[i + 1] != 0 || ps[i + 2] != 1) { i++; continue; }
        uint8_t id = ps[i + 3];
        if (id == 0xBA) {                         /* pack header */
            if ((ps[i + 4] & 0xC0) == 0x40) i += 14 + (ps[i + 13] & 7);   /* MPEG-2 */
            else i += 12;                                                   /* MPEG-1 */
            continue;
        }
        if (id == 0xB9) { i += 4; continue; }     /* program end */
        if (id < 0xBB) { i++; continue; }         /* not a PES/system packet */
        if (i + 6 > n) break;
        size_t len = ((size_t)ps[i + 4] << 8) | ps[i + 5];
        size_t end = i + 6 + len;
        if (end > n) end = n;
        if (id == 0xE0 && (ps[i + 6] & 0xC0) == 0x80) {   /* MPEG-2 PES header */
            size_t p = i + 9 + ps[i + 8];
            if (p < end && w + (end - p) <= cap) { memcpy(out + w, ps + p, end - p); w += end - p; }
        }
        i = end;
    }
    return w;
}

/* Audio: private stream 1 (0xBD). Each payload starts with a 4-byte substream
 * header (ff a1 00 0N, N = the track; the movies carry several language
 * tracks). The track data is an "SShd" header then "SSbd" and PS2 ADPCM.
 * Appends track `track`'s bytes to `out`; returns the count. */
static size_t pss_extract_audio(const uint8_t *ps, size_t n, int track, uint8_t *out, size_t cap)
{
    size_t i = 0, w = 0;
    while (i + 4 <= n) {
        if (ps[i] != 0 || ps[i + 1] != 0 || ps[i + 2] != 1) { i++; continue; }
        uint8_t id = ps[i + 3];
        if (id == 0xBA) { i += ((ps[i + 4] & 0xC0) == 0x40) ? 14 + (ps[i + 13] & 7) : 12; continue; }
        if (id == 0xB9) { i += 4; continue; }
        if (id < 0xBB) { i++; continue; }
        if (i + 6 > n) break;
        size_t len = ((size_t)ps[i + 4] << 8) | ps[i + 5];
        size_t end = i + 6 + len;
        if (end > n) end = n;
        if (id == 0xBD && (ps[i + 6] & 0xC0) == 0x80) {
            size_t p = i + 9 + ps[i + 8];
            if (p + 4 <= end && ps[p] == 0xFF && ps[p + 3] == track) {
                p += 4;
                if (w + (end - p) <= cap) { memcpy(out + w, ps + p, end - p); w += end - p; }
            }
        }
        i = end;
    }
    return w;
}

static uint32_t pss_le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Decodes an SShd/SSbd track (PS2 ADPCM, channels interleaved in blocks of
 * `interleave` bytes) into interleaved 16-bit stereo. Returns frames, or 0. */
static size_t pss_decode_audio(const uint8_t *a, size_t n, int16_t **out, int *rate)
{
    static const int fk[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    if (n < 0x28 || memcmp(a, "SShd", 4) || pss_le32(a + 8) != 0x10) return 0;
    uint32_t hsz = pss_le32(a + 4), ch = pss_le32(a + 16), il = pss_le32(a + 20);
    *rate = (int)pss_le32(a + 12);
    const uint8_t *b = a + 8 + hsz;
    if (b + 8 > a + n || memcmp(b, "SSbd", 4) || ch < 1 || ch > 2 || il < 16 || il % 16) return 0;
    size_t len = pss_le32(b + 4);
    b += 8;
    if (len > (size_t)(a + n - b)) len = (size_t)(a + n - b);
    size_t blocks = len / (il * ch), per = il / 16 * 28;
    size_t frames = blocks * per;
    int16_t *o = (int16_t *)malloc(frames * 4 + 4);
    if (!o) return 0;
    int hist[2][2] = {{0, 0}, {0, 0}};
    for (size_t k = 0; k < blocks; k++)
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *blk = b + (k * ch + c) * il;
            for (uint32_t f = 0; f < il / 16; f++) {
                const uint8_t *fr = blk + f * 16;
                int sh = fr[0] & 15, flt = (fr[0] >> 4) & 7;
                if (flt > 4) flt = 0;
                if (sh > 12) sh = 9;
                for (int s = 0; s < 28; s++) {
                    int nib = (fr[2 + s / 2] >> ((s & 1) * 4)) & 15;
                    if (nib >= 8) nib -= 16;
                    int v = (nib << 12) >> sh;
                    v += (hist[c][0] * fk[flt][0] + hist[c][1] * fk[flt][1] + 32) >> 6;
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    hist[c][1] = hist[c][0];
                    hist[c][0] = v;
                    size_t idx = (k * per + f * 28 + s) * 2;
                    o[idx + c] = (int16_t)v;
                    if (ch == 1) o[idx + 1] = (int16_t)v;
                }
            }
        }
    *out = o;
    return frames;
}
#endif
