/* 989snd (Sony's sound library, EE side) for the menu's sound effects, with a
 * small software mixer in place of the SPU2.
 *
 * Banks are 989snd "SBlk" version 3 banks (sounds -> grains -> tones -> PS2
 * ADPCM waveforms). Loading copies the bank into host memory. Playing a sound
 * starts one voice per "play tone" grain of the sound; voices are decoded and
 * mixed at 48 kHz stereo and streamed to the platform's audio output each
 * vsync (about 100 ms ahead). Volume (0-0x400), pan (degrees) and pitch (1/128 semitone) follow the
 * library's units. Completion callbacks are queued and delivered on the next
 * flush (uyafn_13B620), like the asynchronous library.
 *
 * The SPU2's ADSR envelopes are applied; a stop is a key-off (release).
 * Not done: reverb, the grain types other than "play tone" (random choice,
 * delays...), music streams (uyafn_13CB20) and master volume groups.
 * The menu also calls these every frame and while it waits for disc reads, so
 * they also deliver finished disc reads. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "uya_host.h"
#include "uya_runtime.h"

void uya_cd_service(void);

/* ---- banks ---- */
#define MAX_BANKS 16
typedef struct { u8 *data; u32 size; const u8 *sblk; const u8 *wave; u32 wave_size; } bank_t;
static bank_t banks[MAX_BANKS];          /* handle = index + 1 */

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static s32 bank_add(const u8 *src, u32 size)
{
    if (size < 0x18 || rd32(src + 8) + 4 > size || memcmp(src + rd32(src + 8), "SBlk", 4)) {
        uya_log("sound: not a 989snd bank");
        return 0;
    }
    for (int i = 0; i < MAX_BANKS; i++) {
        if (banks[i].data) continue;
        u8 *d = malloc(size);
        if (!d) return 0;
        memcpy(d, src, size);
        banks[i].data = d;
        banks[i].size = size;
        banks[i].sblk = d + rd32(d + 8);
        banks[i].wave = d + rd32(d + 0x10);
        banks[i].wave_size = rd32(d + 0x14);
        uya_log("sound: bank %d loaded (%u sounds, %u KB of samples)", i + 1, rd16(banks[i].sblk + 0x16), banks[i].wave_size / 1024);
        return i + 1;
    }
    return 0;
}

static u32 bank_size(const u8 *b) { return rd32(b + 0x10) + rd32(b + 0x14); }

/* ---- callbacks, delivered on the next flush ---- */
typedef void (*snd_cb)(s32 handle, u64 user);
static struct { snd_cb cb; s32 h; u64 user; } cbq[256];
static int ncb;

static void queue_cb(void *cb, s32 h, u64 user)
{
    if (!cb || ncb >= 256) return;
    cbq[ncb].cb = (snd_cb)cb; cbq[ncb].h = h; cbq[ncb].user = user; ncb++;
}

static void run_cbs(void)
{
    int n = ncb;
    ncb = 0;
    for (int i = 0; i < n; i++) cbq[i].cb(cbq[i].h, cbq[i].user);
}

/* ---- voices ---- */
#define MAX_VOICES 48
typedef struct {
    int on;
    s32 handle;
    const u8 *data, *end, *loop;
    s32 hist1, hist2;
    s16 buf[28];
    int bufpos;          /* next sample in buf, 28 = decode next frame */
    double frac, step;   /* resampling */
    s16 hist4[4];        /* last four samples for the interpolation */
    int env_phase;       /* 0 attack, 1 decay, 2 sustain, 3 release */
    int env_level, env_count;
    u16 adsr1, adsr2;
    float gl, gr;
    double base_rate;
    int tone_pan;
    float tone_vol;
} voice_t;
static voice_t voices[MAX_VOICES];
static s32 next_handle = 1;

static int decode_frame(voice_t *v)
{
    static const int fk[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    if (v->data + 16 > v->end) return 0;
    const u8 *fr = v->data;
    int sh = fr[0] & 15, flt = (fr[0] >> 4) & 7, flags = fr[1];
    if (flt > 4) flt = 0;
    if (sh > 12) sh = 9;
    if (flags & 4) v->loop = fr;
    for (int s = 0; s < 28; s++) {
        int nib = (fr[2 + s / 2] >> ((s & 1) * 4)) & 15;
        if (nib >= 8) nib -= 16;
        int x = (nib << 12) >> sh;
        x += (v->hist1 * fk[flt][0] + v->hist2 * fk[flt][1] + 32) >> 6;
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        v->hist2 = v->hist1;
        v->hist1 = x;
        v->buf[s] = (s16)x;
    }
    v->bufpos = 0;
    v->data += 16;
    if (flags & 1) {                          /* end of the waveform */
        if ((flags & 2) && v->loop) v->data = v->loop;
        else v->end = v->data;                /* play this frame, then stop */
    }
    return 1;
}

static int next_sample(voice_t *v, s16 *out)
{
    if (v->bufpos >= 28 && !decode_frame(v)) return 0;
    *out = v->buf[v->bufpos++];
    return 1;
}

static void set_mix(voice_t *v, s32 vol, s32 pan, s32 pitch)
{
    int deg = ((pan + v->tone_pan) % 360 + 360) % 360;
    double x = sin((double)deg * 3.14159265358979 / 180.0);   /* -1 left .. 1 right */
    float g = (float)(vol > 0x400 ? 0x400 : (vol < 0 ? 0 : vol)) / 1024.0f * v->tone_vol;
    v->gl = g * (float)sqrt((1.0 - x) * 0.5);
    v->gr = g * (float)sqrt((1.0 + x) * 0.5);
    v->step = v->base_rate * pow(2.0, (double)pitch / 1536.0) / 48000.0;
}

/* rate of a tone: centre note (0x80 and up: 48 kHz base) and fine tune, for note 60 */
static double tone_rate(u8 center, u8 fine)
{
    double base = 44100.0;
    int c = center;
    if (center & 0x80) { c = 0x100 - center; base = 48000.0; }
    return base * pow(2.0, ((60 - c) + fine / 128.0) / 12.0);
}

static s32 play_sound(s32 bank, s32 sound, s32 vol, s32 pan, s32 pitch)
{
    if (bank < 1 || bank > MAX_BANKS || !banks[bank - 1].data) return 0;
    bank_t *b = &banks[bank - 1];
    const u8 *s = b->sblk;
    u32 nsounds = rd16(s + 0x16);
    if (sound < 0 || (u32)sound >= nsounds) return 0;
    const u8 *t1 = s + rd32(s + 0x1C), *t2 = s + rd32(s + 0x20), *t3 = s + rd32(s + 0x34);
    const u8 *se = t1 + sound * 12;
    int ngrains = se[4];
    u32 goff = rd32(se + 8);
    float svol = se[0] / 127.0f;
    s32 h = 0;
    for (int g = 0; g < ngrains; g++) {
        const u8 *gr = t2 + goff + g * 8;
        u32 val = rd32(gr);
        if ((val >> 16) != 0x0100) continue;      /* only "play tone" grains */
        const u8 *tone = t3 + (val & 0xFFFF);
        u32 off = rd32(tone + 0x10), size = rd32(tone + 0x14);
        if (off >= b->wave_size) continue;
        if (off + size > b->wave_size || size == 0) size = b->wave_size - off;
        int vi;
        for (vi = 0; vi < MAX_VOICES && voices[vi].on; vi++) {}
        if (vi == MAX_VOICES) break;
        if (!h) { h = next_handle++; if (next_handle > 0x7FFFFFFF) next_handle = 1; }
        voice_t *v = &voices[vi];
        memset(v, 0, sizeof *v);
        v->on = 1;
        v->handle = h;
        v->data = b->wave + off;
        v->end = b->wave + off + size;
        v->bufpos = 28;
        v->frac = 4.0;                            /* load the first samples */
        v->adsr1 = rd16(tone + 0xA);
        v->adsr2 = rd16(tone + 0xC);
        v->base_rate = tone_rate(tone[2], tone[3]);
        v->tone_vol = svol * ((tone[1] > 127 ? 127 : tone[1]) / 127.0f);
        v->tone_pan = (s16)rd16(tone + 4);
        set_mix(v, vol, pan, pitch);
        if (getenv("UYA_SNDLOG")) uya_log("sound: play bank %d sound %d tone at 0x%X, %.0f Hz, vol %d pan %d pitch %d", bank, sound, off, v->base_rate, vol, pan, pitch);
    }
    return h;
}

static int handle_playing(s32 h)
{
    if (!h || h == -1) return 0;
    for (int i = 0; i < MAX_VOICES; i++) if (voices[i].on && voices[i].handle == h) return 1;
    return 0;
}

/* The SPU2's ADSR envelope, one step per 48 kHz sample (rates as on the SPU). */
static void env_step(voice_t *v)
{
    int rate, inc, expo;
    switch (v->env_phase) {
    case 0: rate = (v->adsr1 >> 8) & 0x7F; inc = 1; expo = v->adsr1 >> 15; break;
    case 1: rate = ((v->adsr1 >> 4) & 0xF) << 2; inc = 0; expo = 1; break;
    case 2: rate = (v->adsr2 >> 6) & 0x7F; inc = !((v->adsr2 >> 14) & 1); expo = v->adsr2 >> 15; break;
    default: rate = (v->adsr2 & 0x1F) << 2; inc = 0; expo = (v->adsr2 >> 5) & 1; break;
    }
    if (v->env_phase == 2 && rate == 0x7F) return;     /* sustain held */
    int shift = rate >> 2;
    int step = inc ? 7 - (rate & 3) : -8 + (rate & 3);
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);
    step <<= (shift < 11 ? 11 - shift : 0);
    if (expo && inc && v->env_level > 0x6000) cycles *= 4;
    if (expo && !inc) step = step * v->env_level >> 15;
    if (++v->env_count < cycles) return;
    v->env_count = 0;
    v->env_level += step;
    if (v->env_level > 0x7FFF) v->env_level = 0x7FFF;
    if (v->env_level < 0) v->env_level = 0;
    if (v->env_phase == 0 && v->env_level >= 0x7FFF) v->env_phase = 1;
    else if (v->env_phase == 1 && v->env_level <= (((v->adsr1 & 0xF) + 1) * 0x800)) v->env_phase = 2;
    else if (v->env_phase == 3 && v->env_level == 0) v->on = 0;
}

/* Mixes `frames` stereo frames (48 kHz). Resampling is 4-point cubic
 * B-spline, a smoothing filter like the SPU's Gaussian interpolation. */
static void mix(s16 *out, int frames)
{
    for (int f = 0; f < frames; f++) {
        float l = 0, r = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            voice_t *v = &voices[i];
            if (!v->on) continue;
            while (v->frac >= 1.0) {
                s16 x;
                if (!next_sample(v, &x)) { v->on = 0; break; }
                v->hist4[0] = v->hist4[1]; v->hist4[1] = v->hist4[2]; v->hist4[2] = v->hist4[3]; v->hist4[3] = x;
                v->frac -= 1.0;
            }
            if (!v->on) continue;
            float t = (float)v->frac, t2 = t * t, t3 = t2 * t;
            float w0 = (1 - t) * (1 - t) * (1 - t) / 6.0f;
            float w1 = (3 * t3 - 6 * t2 + 4) / 6.0f;
            float w2 = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.0f;
            float w3 = t3 / 6.0f;
            float s = v->hist4[0] * w0 + v->hist4[1] * w1 + v->hist4[2] * w2 + v->hist4[3] * w3;
            env_step(v);
            s *= v->env_level / 32768.0f;
            l += s * v->gl;
            r += s * v->gr;
            v->frac += v->step;
        }
        out[f * 2] = (s16)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        out[f * 2 + 1] = (s16)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
}

/* RAC1: uya_sound_pump (called once per vsync) is in host/src/rac1/snd989.c,
 * which mixes the host IOP driver's voices; the UYA mixer above is unused. */

/* ---- the library calls ---- */

/* 0x0013B620: flush queued commands; 0 = nothing pending. Delivers callbacks. */
s32 uyafn_13B620(void) { uya_cd_service(); run_cbs(); return 0; }
/* 0x0013CA28, 0x0013CA20: per-frame library service. */
void uyafn_13CA28(void) { uya_cd_service(); }
void uyafn_13CA20(void) { uya_cd_service(); }

/* 0x0013BC30: load a bank from EE memory; cb(handle, user) when done. */
s32 uyafn_13BC30(u32 addr, void *cb, u64 user)
{
    const u8 *src = (const u8 *)(uintptr_t)addr;
    s32 h = bank_add(src, bank_size(src));
    queue_cb(cb, h, user);
    return h;
}

/* 0x0013BAB8: load a bank from the disc at sector lsn; cb(handle, user) when done. */
s32 uyafn_13BAB8(s32 lsn, s32 offset, void *cb, u64 user)
{
    u8 head[2048];
    s32 h = 0;
    (void)offset;
    if (uya_disc_read((u32)lsn, 1, head) == 0) {
        u32 size = bank_size(head);
        u32 sectors = (size + 2047) / 2048;
        u8 *b = (size > 0x18 && size < (16u << 20)) ? malloc((size_t)sectors * 2048) : 0;
        if (b && uya_disc_read((u32)lsn, sectors, b) == 0) h = bank_add(b, size);
        free(b);
    }
    queue_cb(cb, h, user);
    return h;
}

/* 0x0013C0B8: play `sound` from `bank` with volume, pan, pitch mod and pitch bend. */
s32 uyafn_13C0B8(s32 bank, s32 sound, s32 vol, s32 pan, s32 pitch, s32 pb, void *cb, u32 user)
{
    (void)pb;
    s32 h = play_sound(bank, sound, vol, pan, pitch);
    queue_cb(cb, h, user);
    return h;
}

/* 0x0013C2C0: update a playing sound (mode bits: 1 volume, 2 pan, 8 pitch, 0x10 bend). */
s32 uyafn_13C2C0(s32 h, s32 mode, s32 vol, s32 pan, s32 pitch, s32 pb, void *cb, u32 user)
{
    (void)mode; (void)pb;
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].on && voices[i].handle == h) set_mix(&voices[i], vol, pan, pitch);
    queue_cb(cb, handle_playing(h) ? h : 0, user);
    return 0;
}

/* 0x0013C200: is the sound still playing? cb(handle or 0, user). */
s32 uyafn_13C200(s32 h, void *cb, u32 user)
{
    queue_cb(cb, handle_playing(h) ? h : 0, user);
    return 0;
}

/* 0x0013C118: stop a sound. */
void uyafn_13C118(s32 h)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].on && voices[i].handle == h) { voices[i].env_phase = 3; voices[i].env_count = 0; }   /* key off: release */
}

/* 0x0013C170: stop every sound. */
void uyafn_13C170(void) { for (int i = 0; i < MAX_VOICES; i++) voices[i].on = 0; }
/* 0x0013BF00, 0x0013BED0: unload a bank (kept: the handle is not reused). */
void uyafn_13BF00(s32 h) { (void)h; }
void uyafn_13BED0(s32 h) { (void)h; }

/* 0x0013BF30: master volume of a group (not applied yet). */
void uyafn_13BF30(s32 group, s32 vol) { (void)group; (void)vol; }
/* 0x0013CF80, 0x0013CFF8: reverb set-up (not emulated). */
void uyafn_13CF80(s32 a, s32 b, s32 c, s32 d, s32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void uyafn_13CFF8(s32 a, s32 b, s32 c, s32 d) { (void)a; (void)b; (void)c; (void)d; }
/* 0x0013D1E8: distance to pitch shift (Doppler); none. */
s32 uyafn_13D1E8(s32 d) { (void)d; return 0; }

/* 0x0013CAE8 and 0x0013CD00(n): 989snd commands 0x34 and 0x36, which the menu
 * sends after stopping every sound when a movie starts (uyafn_39CBA0). They
 * act on the music stream player, which the host does not have yet, so both
 * succeed and do nothing. */
s32 uyafn_13CAE8(void) { return 0; }
s32 uyafn_13CD00(s32 n) { (void)n; return 0; }
