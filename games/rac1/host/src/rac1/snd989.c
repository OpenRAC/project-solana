/* 989snd's IOP side for RAC1, written for the host.
 *
 * The EE half of the library (src/core/989snd.c) is compiled from the decomp;
 * it talks to the sound driver on the IOP through two transports,
 * snd_SendIOPCommandAndWait (0x0012E688) and snd_SendIOPCommandNoWait
 * (0x0012E820), whose host versions (host/overrides) hand each command to
 * rac1_snd_command() here. The bank loads (0x0012E060 by disc location,
 * 0x0012E1C8 from EE memory) and snd_FlushSoundCommands (0x0012DDC0) are host
 * versions too. Replies that come with a callback are delivered at the next
 * snd_FlushSoundCommands, the way the IOP's replies arrive one batch later.
 *
 * What the driver does, as 989snd does it on the IOP:
 *  - banks: 989snd "SBlk" version 1 sound blocks (sounds -> 0x28-byte grains,
 *    tones with PS-ADPCM samples), copied to host memory;
 *  - sounds: one handler per playing sound runs the sound's grain program at
 *    240 ticks a second (tones, delays, random choices, registers, markers,
 *    loops, LFOs, child sounds, ...); volume, pan, pitch modulation and pitch
 *    bend in the library's units (0-0x400, degrees, 1/128 semitone, +-0x7FFF);
 *    master volume per volume group; pause/continue by group;
 *  - VAG streams (the game's music and dialog): VAGp files read from the disc
 *    as they play, started paused or not, queued after another stream (the
 *    music's intro then its loop), looped, paused, faded; time remaining,
 *    "buffered" and "still playing" replies;
 *  - the stream-safe CD reads (command 0x38): the game's own streamed loads,
 *    once VAG streaming is on, ask the IOP to read the disc into EE memory;
 *  - a software SPU2: 48 voices with PS-ADPCM decoding, loop flags, the
 *    hardware ADSR envelope, the 4-tap Gaussian interpolation and pitch in SPU
 *    units (0x1000 = 48 kHz), mixed at 48 kHz stereo and handed to the
 *    platform's audio output at every vsync (host/src/platform_*.c);
 *  - the SPU2's reverb (libsd's effect modes) on the voices that send to it.
 *
 * The grain semantics, the volume curve (MakeVolume/AdjustVolToGroup), the pan
 * and pitch tables and the note-to-pitch conversion follow the jak-project's
 * reimplementation of 989snd (ISC licence, see THIRD_PARTY_NOTICES.md).
 *
 * Not done: the noise generator (tones with flag 8), the movie sound commands
 * (0x3B-0x3E, 0x5A, 0x5B; the movie player plays its own sound track). */
#include "uya_host.h"
#include "uya_runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snd989_tables.h"

#define EE(a) ((u8 *)(uintptr_t)(u32)(a))
#define EEW(a) (*(volatile s32 *)(uintptr_t)(u32)(a))

static int snd_log = -1;
uint32_t uya_frame_count(void);
#define SLOG(fmt, ...) do { if (snd_log > 0) uya_log("v%u " fmt, uya_frame_count(), ##__VA_ARGS__); } while (0)

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static s32 clampi(s32 v, s32 lo, s32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
static s32 wrap360(s32 p) { p %= 360; return p < 0 ? p + 360 : p; }

/* the IOP's random numbers (989snd uses the C library's rand on the IOP) */
static u32 rng = 1;
static s32 snd_rand(void) { rng = rng * 1103515245u + 12345u; return (s32)((rng >> 16) & 0x7FFF); }

#define VOLUME_DONT_CHANGE 0x7FFFFFFF
#define PAN_RESET (-1)
#define PAN_DONT_CHANGE (-2)

/* ===================================================================== */
/* SPU2 voices                                                            */
/* ===================================================================== */

typedef struct {
    s8 priority, vol, center_note, center_fine;
    s16 pan;
    s8 map_low, map_high, pb_low, pb_high;
    u16 adsr1, adsr2, flags;
    u32 sample;                      /* offset in the bank's sample data */
} tone_t;

struct stream;

enum { PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE, PH_STOPPED };

typedef struct {
    int on;
    int handler;                     /* sound handler index, -1 for none */
    struct stream *stream;           /* the VAG stream feeding it */
    const u8 *data;                  /* PS-ADPCM data */
    u32 size;                        /* its size in bytes */
    u32 nax, lsa;                    /* current block, loop start (byte offsets) */
    int sub;                         /* next sample in the block, 28 = new block */
    int first;
    u8 shift, filter, flags;
    s32 h1, h2;                      /* ADPCM history */
    s16 hist[4];                     /* last four decoded samples, oldest first */
    u32 counter;                     /* pitch counter (12-bit fraction) */
    u16 pitch;
    /* ADSR */
    int phase, env_exp, env_dec, env_shift, env_step;
    s32 level, target;
    u32 env_counter;
    u16 adsr1, adsr2;
    s16 voll, volr;                  /* SPU volume registers (fixed mode) */
    int paused;
    /* 989snd's view of the voice */
    tone_t tone;
    u8 group;
    s32 g_vol, g_pan;
    s16 base_l, base_r;
    s32 start_note, start_fine, cur_pm, cur_pb;
    u32 age;
} voice_t;

#define NVOICES 48
static voice_t voices[NVOICES];
static u32 voice_age;

/* ---- ADSR (the SPU's envelope: one step per 48 kHz sample) ---- */
static void adsr_settings(voice_t *v)
{
    u32 r = v->adsr1 | ((u32)v->adsr2 << 16);
    switch (v->phase) {
    case PH_ATTACK:
        v->env_exp = (r >> 15) & 1; v->env_dec = 0; v->env_shift = (r >> 10) & 0x1F;
        v->env_step = 7 - (int)((r >> 8) & 3); v->target = 0x7FFF; break;
    case PH_DECAY:
        v->env_exp = 1; v->env_dec = 1; v->env_shift = (r >> 4) & 0xF; v->env_step = -8;
        v->target = (s32)(((r & 0xF) + 1) << 11); break;
    case PH_SUSTAIN:
        v->env_exp = (r >> 31) & 1; v->env_dec = (r >> 30) & 1; v->env_shift = (r >> 24) & 0x1F;
        v->env_step = v->env_dec ? -8 + (int)((r >> 22) & 3) : 7 - (int)((r >> 22) & 3); v->target = 0; break;
    case PH_RELEASE:
        v->env_exp = (r >> 21) & 1; v->env_dec = 1; v->env_shift = (r >> 16) & 0x1F; v->env_step = -8;
        v->target = 0; break;
    default: break;
    }
}

static void adsr_run(voice_t *v)
{
    if (v->phase == PH_STOPPED) return;
    u32 cstep = 0x800000;
    int sh = v->env_shift - 11;
    if (sh > 0) cstep >>= sh;
    s32 step = (s16)(v->env_step << (11 - v->env_shift > 0 ? 11 - v->env_shift : 0));
    if (v->env_exp) {
        if (!v->env_dec && v->level > 0x6000) cstep >>= 2;
        if (v->env_dec) step = (s16)((step * v->level) >> 15);
    }
    v->env_counter += cstep;
    if (v->env_counter >= 0x800000) {
        v->env_counter = 0;
        v->level = clampi(v->level + step, 0, 0x7FFF);
    }
    if (v->phase == PH_SUSTAIN) return;
    if ((!v->env_dec && v->level >= v->target) || (v->env_dec && v->level <= v->target)) {
        if (v->phase == PH_ATTACK) v->phase = PH_DECAY;
        else if (v->phase == PH_DECAY) v->phase = PH_SUSTAIN;
        else if (v->phase == PH_RELEASE) v->phase = PH_STOPPED;
        adsr_settings(v);
    }
}

static void voice_key_off(voice_t *v)
{
    if (v->phase == PH_STOPPED) return;
    v->phase = PH_RELEASE;
    v->env_counter = 0;
    adsr_settings(v);
}

static void voice_kill(voice_t *v) { v->phase = PH_STOPPED; v->level = 0; }

static const u8 *stream_block(struct stream *s, u32 off);   /* below */

/* reads the block header at v->nax */
static int voice_block_header(voice_t *v)
{
    const u8 *b;
    if (v->stream) b = stream_block(v->stream, v->nax);
    else b = v->nax + 16 <= v->size ? v->data + v->nax : 0;
    if (!b) return 0;
    v->shift = b[0] & 0xF;
    v->filter = (b[0] >> 4) & 0x7;
    v->flags = b[1];
    if (v->flags & 4) v->lsa = v->nax;
    return 1;
}

static void stream_voice_end(voice_t *v);    /* below */

static s16 voice_next_sample(voice_t *v)
{
    static const int fk[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    if (v->phase == PH_STOPPED) return 0;
    if (v->sub >= 28) {
        if (!v->first) {
            if (v->stream) {
                if (v->flags & 1) stream_voice_end(v);    /* the end of the stream's data */
                else v->nax += 16;
                if (v->phase == PH_STOPPED) return 0;
            } else if (v->flags & 1) {                    /* loop end */
                v->nax = v->lsa;
                if (!(v->flags & 2)) { voice_kill(v); return 0; }
            } else v->nax += 16;
        }
        v->first = 0;
        if (!voice_block_header(v)) {
            if (v->stream) { stream_voice_end(v); if (v->phase == PH_STOPPED || !voice_block_header(v)) { voice_kill(v); return 0; } }
            else { voice_kill(v); return 0; }
        }
        v->sub = 0;
    }
    const u8 *b = v->stream ? stream_block(v->stream, v->nax) : v->data + v->nax;
    if (!b) { voice_kill(v); return 0; }
    int nib = (b[2 + v->sub / 2] >> ((v->sub & 1) * 4)) & 0xF;
    s32 s = (s16)(nib << 12);
    int shift = v->shift > 12 ? 9 : v->shift;
    int f = v->filter > 4 ? 0 : v->filter;
    s >>= shift;
    s += (fk[f][0] * v->h1) >> 6;
    s += (fk[f][1] * v->h2) >> 6;
    s = clampi(s, -32768, 32767);
    v->h2 = v->h1;
    v->h1 = s;
    v->sub++;
    return (s16)s;
}

static void voice_start(voice_t *v)
{
    v->nax = 0;
    v->lsa = 0;
    v->sub = 28;
    v->first = 1;
    v->h1 = v->h2 = 0;
    memset(v->hist, 0, sizeof v->hist);
    v->counter = 0;
    v->level = 0;
    v->env_counter = 0;
    v->phase = PH_ATTACK;
    adsr_settings(v);
    v->paused = 0;
    v->age = ++voice_age;
    v->on = 1;
}

/* one output sample of a voice; returns 0 when it has nothing to say */
static void voice_run(voice_t *v, s32 *l, s32 *r)
{
    s32 s = 0;
    const short *w = snd_interp[(v->counter & 0xFF0) >> 4];
    s = (s16)(s + ((v->hist[0] * w[0]) >> 15));
    s = (s16)(s + ((v->hist[1] * w[1]) >> 15));
    s = (s16)(s + ((v->hist[2] * w[2]) >> 15));
    s = (s16)(s + ((v->hist[3] * w[3]) >> 15));
    u32 step = v->paused ? 0 : (v->pitch > 0x3FFF ? 0x3FFF : v->pitch);
    v->counter += step;
    u32 n = v->counter >> 12;
    v->counter &= 0xFFF;
    while (n--) {
        v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
        v->hist[3] = voice_next_sample(v);
    }
    s = (s * v->level) >> 15;
    if (!v->paused) {
        *l += (s * (v->voll << 1)) >> 15;
        *r += (s * (v->volr << 1)) >> 15;
    }
    adsr_run(v);
}

/* ===================================================================== */
/* 989snd's volume and pitch rules                                        */
/* ===================================================================== */

static s32 master_vol[32];
static s32 stereo_mode;              /* snd_SetPlaybackMode: 1 = mono */

static void make_volume(int vol1, int pan1, int vol2, int pan2, int vol3, int pan3, s16 *L, s16 *R)
{
    s32 vol = vol1 * 258;
    vol = vol * vol2 / 0x7F;
    vol = vol * vol3 / 0x7F;
    if (vol == 0) { *L = *R = 0; return; }
    if (stereo_mode == 1) { *L = *R = (s16)vol; return; }
    int p = wrap360(wrap360(pan1) + wrap360(pan2) + wrap360(pan3));
    if (p >= 270) p -= 270; else p += 90;
    if (p < 180) { *L = (s16)(snd_pan_table[p][0] * vol / 0x3FFF); *R = (s16)(snd_pan_table[p][1] * vol / 0x3FFF); }
    else { *R = (s16)(snd_pan_table[p - 180][0] * vol / 0x3FFF); *L = (s16)(snd_pan_table[p - 180][1] * vol / 0x3FFF); }
}

static s16 adjust_vol_to_group(s16 in, int group)
{
    s32 v = in;
    if (group >= 15) return (s16)v;
    if (v >= 0x7FFF) v = 0x7FFE;
    v = v * master_vol[group] / 0x400;
    int sign = v < 0 ? -1 : 1;
    return (s16)((v * v) / 0x7FFE * sign);
}

static u16 note2pitch(u16 center_note, u16 center_fine, u16 note, short fine)
{
    s32 _fine = fine + (u16)center_fine, _fine2 = _fine, _note, offset1, offset2, val, val2, val3, ret;
    if (_fine < 0) _fine2 = _fine + 127;
    _fine2 = _fine2 / 128;
    _note = note + _fine2 - center_note;
    val3 = _note / 6;
    if (_note < 0) val3--;
    offset2 = _fine - _fine2 * 128;
    val2 = _note < 0 ? -1 : 0;
    if (val3 < 0) val3--;
    val2 = (val3 / 2) - val2;
    val = val2 - 2;
    offset1 = _note - (val2 * 12);
    if ((offset1 < 0) || ((offset1 == 0) && (offset2 < 0))) { offset1 = offset1 + 12; val = val2 - 3; }
    if (offset2 < 0) { offset1 = (offset1 - 1) + _fine2; offset2 += (_fine2 + 1) * 128; }
    if (offset1 < 0) offset1 = 0;
    if (offset1 > 11) offset1 = 11;
    if (offset2 < 0) offset2 = 0;
    if (offset2 > 127) offset2 = 127;
    ret = (snd_note_pitch[offset1] * snd_note_pitch[offset2 + 12]) / 0x10000;
    if (val < 0) ret = (ret + (1 << (-val - 1))) >> -val;
    return (u16)ret;
}

static u16 ps1_note2pitch(s8 center_note, s8 center_fine, short note, short fine)
{
    int ps1 = center_note >= 0;
    if (!ps1) center_note = -center_note;
    u16 p = note2pitch((u16)center_note, (u16)(u8)center_fine, (u16)note, fine);
    if (ps1) p = (u16)(44100 * p / 48000);
    return p;
}

static void pitch_bend(const tone_t *t, int pb, int pm, int start_note, int start_fine, s16 *note, s16 *fine)
{
    s32 v9 = (start_note << 7) + start_fine + pm, v7;
    if (pb >= 0) v7 = t->pb_high * (pb << 7) / 0x7FFF + v9;
    else v7 = t->pb_low * (pb << 7) / 0x8000 + v9;
    *note = (s16)(v7 / 128);
    *fine = (s16)(v7 % 128);
}

static void voice_apply_volume(voice_t *v)
{
    v->voll = (s16)(adjust_vol_to_group(v->base_l, v->group) >> 1);
    v->volr = (s16)(adjust_vol_to_group(v->base_r, v->group) >> 1);
}

static void voice_apply_pitch(voice_t *v)
{
    s16 note, fine;
    pitch_bend(&v->tone, v->cur_pb, v->cur_pm, v->start_note, v->start_fine, &note, &fine);
    v->pitch = ps1_note2pitch(v->tone.center_note, v->tone.center_fine, note, fine);
}

/* ===================================================================== */
/* banks                                                                  */
/* ===================================================================== */

typedef struct {
    int used;
    u32 handle;
    u8 *data;
    u32 size;
    u8 *sblk;                        /* the SBlk chunk */
    u32 sblk_size;
    const u8 *smp;                   /* sample data */
    u32 smp_size;
    int nsounds;
    u32 first_sound, first_grain;
} bank_t;

#define NBANKS 16
static bank_t banks[NBANKS];
static u32 bank_serial;

static u32 bank_file_size(const u8 *b)
{
    u32 n = rd32(b + 4), end = 0;
    if (n == 0 || n > 4) return 0;
    for (u32 i = 0; i < n; i++) {
        u32 e = rd32(b + 8 + i * 8) + rd32(b + 12 + i * 8);
        if (e > end) end = e;
    }
    return end;
}

static u32 bank_add(const u8 *src, u32 size)
{
    if (size < 0x18 || size > (32u << 20)) { uya_log("989snd: bad bank size %u", size); return 0; }
    u32 so = rd32(src + 8), ss = rd32(src + 12);
    if (so + 0x40 > size || memcmp(src + so, "SBlk", 4)) { uya_log("989snd: not a 989snd bank"); return 0; }
    for (int i = 0; i < NBANKS; i++) {
        bank_t *b = &banks[i];
        if (b->used) continue;
        u8 *d = malloc(size);
        if (!d) return 0;
        memcpy(d, src, size);
        memset(b, 0, sizeof *b);
        b->used = 1;
        b->data = d;
        b->size = size;
        b->sblk = d + so;
        b->sblk_size = ss;
        if (rd32(src + 4) >= 2) { b->smp = d + rd32(src + 16); b->smp_size = rd32(src + 20); }
        if (b->smp && b->smp + b->smp_size > d + size) b->smp_size = (u32)(d + size - b->smp);
        b->nsounds = (s16)rd16(b->sblk + 0x16);
        b->first_sound = rd32(b->sblk + 0x1C);
        b->first_grain = rd32(b->sblk + 0x20);
        b->handle = 0x00120000u + (u32)i * 0x1000u + ((++bank_serial) & 0xFFF);
        if (rd32(b->sblk + 4) != 1) uya_log("989snd: SBlk version %u (only 1 is expected)", rd32(b->sblk + 4));
        uya_log("989snd: bank %08X loaded: %d sounds, %u KB of samples", b->handle, b->nsounds, b->smp_size / 1024);
        if (snd_log > 0 && getenv("UYA_DUMP")) {               /* for checking: the bank as loaded */
            char path[512];
            snprintf(path, sizeof path, "%s/bank_%08X.bin", getenv("UYA_DUMP"), b->handle);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(d, 1, size, f); fclose(f); }
        }
        return b->handle;
    }
    uya_log("989snd: no room for another bank");
    return 0;
}

static bank_t *bank_get(u32 h)
{
    for (int i = 0; i < NBANKS; i++) if (banks[i].used && banks[i].handle == h) return &banks[i];
    return 0;
}

static const u8 *sfx_entry(const bank_t *b, int sfx) { return b->sblk + b->first_sound + sfx * 12; }
static const u8 *grain_at(const bank_t *b, int sfx, int g) { return b->sblk + b->first_grain + rd32(sfx_entry(b, sfx) + 8) + g * 0x28; }
static int sfx_ngrains(const bank_t *b, int sfx) { return (s8)sfx_entry(b, sfx)[4]; }

static void read_tone(const u8 *p, tone_t *t)
{
    t->priority = (s8)p[0]; t->vol = (s8)p[1]; t->center_note = (s8)p[2]; t->center_fine = (s8)p[3];
    t->pan = (s16)rd16(p + 4);
    t->map_low = (s8)p[6]; t->map_high = (s8)p[7]; t->pb_low = (s8)p[8]; t->pb_high = (s8)p[9];
    t->adsr1 = rd16(p + 0xA); t->adsr2 = rd16(p + 0xC); t->flags = rd16(p + 0xE);
    t->sample = rd32(p + 0x10);
}

/* ===================================================================== */
/* sound handlers (989snd's "block sound" player)                         */
/* ===================================================================== */

typedef struct {
    int type, target, target_extra, setup_flags;
    s32 depth, next_step, step_size, state_hold1, state_hold2, range, last_lfo;
    u32 tick;
} lfo_t;

enum { LFO_OFF, LFO_SINE, LFO_SQUARE, LFO_TRIANGLE, LFO_SAW, LFO_RAND };
enum { LT_NONE, LT_VOLUME, LT_PAN, LT_PMOD, LT_PBEND };

typedef struct {
    int used;
    u32 handle;                      /* the handle the EE knows (shared by children) */
    int parent;                      /* parent handler index, -1 for a top-level sound */
    bank_t *bank;
    int sfx, orig_sfx;
    u8 group;
    int done, paused;
    int next_grain, countdown;
    int grains_to_play, grains_to_skip, skip_grains;
    s32 orig_volume, orig_pan, cur_volume, cur_pan, cur_pm, cur_pb;
    s32 app_volume, app_pan, app_pm, app_pb;
    s32 lfo_volume, lfo_pan, lfo_pm, lfo_pb;
    int note, fine;
    s8 regs[4];
    lfo_t lfo[4];
    u32 start_tick;
} handler_t;

#define NHANDLERS 160
static handler_t handlers[NHANDLERS];
static s8 g_block_reg[32];
static u32 snd_tick;
static u32 next_handle = 1;

static u32 new_handle(void)
{
    u32 h = next_handle++;
    if (next_handle >= 0x7FFFFFF0u) next_handle = 1;
    return h;
}

static int handler_has_voices(int hi)
{
    for (int i = 0; i < NVOICES; i++) if (voices[i].on && voices[i].handler == hi) return 1;
    return 0;
}

static int handler_has_children(int hi)
{
    for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].parent == hi) return 1;
    return 0;
}

static void handler_free(int hi)
{
    for (int i = 0; i < NHANDLERS; i++)
        if (handlers[i].used && handlers[i].parent == hi) handler_free(i);
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].on && voices[i].handler == hi) { voice_kill(&voices[i]); voices[i].handler = -1; }
    handlers[hi].used = 0;
}

static void handler_set_vol_pan(int hi, s32 vol, s32 pan);
static void handler_update_pitch(int hi);

static s32 sine_lfo(int step) { return (s32)floor(32767.0 * sin((double)step * 6.283185307179586 / 2048.0) + 0.5); }   /* not lrint: the game thread truncates */

static s32 lfo_get(lfo_t *l, s32 mult)
{
    s32 step = l->next_step >> 16, ret = 0;
    l->next_step += mult * l->step_size;
    if (l->next_step > 0x7FFFFFF) l->next_step -= 0x8000000;
    step &= 2047;
    switch (l->type) {
    case LFO_SINE: ret = sine_lfo(step); break;
    case LFO_SQUARE: ret = step >= l->state_hold1 ? -32767 : 32767; break;
    case LFO_TRIANGLE:
        if (step < 512) ret = 0x7FFF * step / 512;
        else if (step >= 1536) ret = 0x7FFF * (step - 1536) / 512 - 0x7FFF;
        else ret = 0x7FFF - 65534 * (step - 512) / 1024;
        break;
    case LFO_SAW: ret = step >= 1024 ? 0x7FFF * (step - 1024) / 1024 - 0x7FFF : 0x7FFF * step / 1023; break;
    case LFO_RAND:
        if (step >= 1024 && l->state_hold2 == 1) { l->state_hold2 = 0; l->state_hold1 = 2 * ((snd_rand() & 0x7FFF) - 0x3FFF); }
        else if (step < 1024 && l->state_hold2 == 0) { l->state_hold2 = 1; l->state_hold1 = -(snd_rand() & 0x7FFF) * (snd_rand() & 1); }
        ret = l->state_hold1;
        break;
    default: ret = 0; break;
    }
    if (l->setup_flags & 1) ret = -ret;
    return ret;
}

static void lfo_calc_depth(int hi, lfo_t *l)
{
    handler_t *h = &handlers[hi];
    if (l->target == LT_VOLUME) l->range = ((s8)sfx_entry(h->bank, h->sfx)[0] * l->depth) >> 10;
    if (l->target == LT_PAN) l->range = (180 * l->depth) >> 10;
    if (l->target == LT_PMOD) l->range = (6096 * l->depth) >> 10;
    if (l->target == LT_PBEND) l->range = (0x7FFF * l->depth) >> 10;
    l->last_lfo = 0;
}

static void lfo_tick(int hi, lfo_t *l)
{
    handler_t *h = &handlers[hi];
    l->tick++;
    if (l->target == LT_NONE || l->type == LFO_OFF || (l->tick & 1) == 0) return;
    switch (l->target) {
    case LT_VOLUME: {
        s32 v = (l->range * (lfo_get(l, 2) - 0x7FFF)) >> 16;
        if (h->lfo_volume != v) { h->lfo_volume = v; handler_set_vol_pan(hi, VOLUME_DONT_CHANGE, PAN_DONT_CHANGE); }
    } break;
    case LT_PAN: {
        s32 p = (l->range * lfo_get(l, 2)) >> 15;
        if (h->lfo_pan != p) { h->lfo_pan = p; handler_set_vol_pan(hi, VOLUME_DONT_CHANGE, PAN_DONT_CHANGE); }
    } break;
    case LT_PMOD: {
        s32 pm = (lfo_get(l, 2) * l->range) >> 15;
        if (h->lfo_pm != pm) { h->lfo_pm = pm; handler_update_pitch(hi); }
    } break;
    case LT_PBEND: {
        s32 pb = (lfo_get(l, 2) * l->range) >> 15;
        if (h->lfo_pb != pb) { h->lfo_pb = pb; handler_update_pitch(hi); }
    } break;
    default: break;
    }
}

static void handler_set_vol_pan(int hi, s32 vol, s32 pan)
{
    handler_t *h = &handlers[hi];
    if (vol >= 0) { if (vol != VOLUME_DONT_CHANGE) h->app_volume = vol; }
    else h->app_volume = -1024 * vol / 127;
    if (pan == PAN_RESET) h->app_pan = (s16)rd16(sfx_entry(h->bank, h->sfx) + 2);
    else if (pan != PAN_DONT_CHANGE) h->app_pan = pan;
    s32 nv = clampi(((h->app_volume * h->orig_volume) >> 10) + h->lfo_volume, 0, 127);
    s32 np = wrap360(h->app_pan % 360 + h->lfo_pan);
    if (np != h->cur_pan || nv != h->cur_volume) {
        h->cur_volume = nv;
        h->cur_pan = np;
        for (int i = 0; i < NHANDLERS; i++)
            if (handlers[i].used && handlers[i].parent == hi) handler_set_vol_pan(i, h->app_volume * h->orig_volume / 127, pan);
        for (int i = 0; i < NVOICES; i++) {
            voice_t *v = &voices[i];
            if (!v->on || v->handler != hi) continue;
            make_volume(127, 0, h->cur_volume, h->cur_pan, v->g_vol, v->g_pan, &v->base_l, &v->base_r);
            voice_apply_volume(v);
        }
    }
}

static void handler_update_pitch(int hi)
{
    handler_t *h = &handlers[hi];
    h->cur_pm = h->app_pm + h->lfo_pm;
    h->cur_pb = clampi(h->app_pb + h->lfo_pb, -32768, 32767);
    for (int i = 0; i < NVOICES; i++) {
        voice_t *v = &voices[i];
        if (!v->on || v->handler != hi) continue;
        v->cur_pm = h->cur_pm;
        v->cur_pb = h->cur_pb;
        voice_apply_pitch(v);
    }
}

static void handler_set_pm(int hi, s32 pm)
{
    for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].parent == hi) handler_set_pm(i, pm);
    handlers[hi].app_pm = pm;
    handler_update_pitch(hi);
}

static void handler_set_pb(int hi, s32 pb)
{
    for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].parent == hi) handler_set_pb(i, pb);
    handlers[hi].app_pb = pb;
    handler_update_pitch(hi);
}

static void handler_stop(int hi)
{
    handlers[hi].done = 1;
    for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].parent == hi) handler_stop(i);
    for (int i = 0; i < NVOICES; i++) if (voices[i].on && voices[i].handler == hi) voice_key_off(&voices[i]);
}

static void handler_pause(int hi, int pause)
{
    handlers[hi].paused = pause;
    for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].parent == hi) handler_pause(i, pause);
    for (int i = 0; i < NVOICES; i++) {
        voice_t *v = &voices[i];
        if (!v->on || v->handler != hi) continue;
        v->paused = pause;
        if (!pause) { voice_apply_volume(v); voice_apply_pitch(v); }
    }
}

/* a free SPU voice, or the one to steal (released first, then the lowest priority, then the oldest) */
static voice_t *voice_alloc(int priority)
{
    voice_t *best = 0;
    for (int i = 0; i < NVOICES; i++) {
        voice_t *v = &voices[i];
        if (!v->on) return v;
        if (v->stream) continue;
        if (!best) { best = v; continue; }
        int vr = v->phase >= PH_RELEASE, br = best->phase >= PH_RELEASE;
        if (vr != br) { if (vr) best = v; continue; }
        if (v->tone.priority != best->tone.priority) { if (v->tone.priority < best->tone.priority) best = v; continue; }
        if (v->age < best->age) best = v;
    }
    if (best && best->phase < PH_RELEASE && best->tone.priority > priority) return 0;
    return best;
}

static s32 reg_value(handler_t *h, s32 x, int pan)
{
    if (x >= 0) return x;
    s32 r;
    if (x >= -4) r = h->regs[-x - 1];
    else if (x == -5) return pan ? snd_rand() % 360 : snd_rand() % 0x7F;
    else r = g_block_reg[(-x - 6) & 31];
    return pan ? 360 * r / 127 : r;
}

static void grain_tone(int hi, const u8 *gp)
{
    handler_t *h = &handlers[hi];
    tone_t t;
    read_tone(gp + 8, &t);
    h->cur_volume = clampi(((h->app_volume * h->orig_volume) >> 10) + h->lfo_volume, 0, 127);
    h->cur_pan = wrap360(h->app_pan % 360 + h->lfo_pan);
    if (t.flags & 8) return;                     /* noise: not done */
    if (!h->bank->smp || t.sample >= h->bank->smp_size) return;
    s32 vol = t.vol;
    if (vol < 0) vol = reg_value(h, vol, 0);
    if (vol < 0) vol = 0;
    s32 pan = t.pan;
    if (pan < 0) {
        if (pan >= -4) pan = 360 * h->regs[-pan - 1] / 127;
        else if (pan == -5) pan = snd_rand() % 360;
        else pan = 360 * g_block_reg[(-pan - 6) & 31] / 127;
    }
    pan = wrap360(pan);
    voice_t *v = voice_alloc(t.priority);
    if (!v) return;
    if (v->on && v->handler >= 0) { /* stolen */ }
    memset(v, 0, sizeof *v);
    v->handler = hi;
    v->data = h->bank->smp + t.sample;
    v->size = h->bank->smp_size - t.sample;
    v->tone = t;
    v->group = h->group;
    v->g_vol = vol;
    v->g_pan = pan;
    v->start_note = h->note;
    v->start_fine = h->fine;
    v->cur_pb = h->cur_pb;
    v->cur_pm = h->cur_pm;
    v->adsr1 = t.adsr1;
    v->adsr2 = t.adsr2;
    make_volume(127, 0, h->cur_volume, h->cur_pan, vol, pan, &v->base_l, &v->base_r);
    voice_apply_volume(v);
    voice_apply_pitch(v);
    voice_start(v);
    if (h->paused) v->paused = 1;
}

static int handler_new(bank_t *b, int sfx, s32 sfx_vol, s32 sfx_pan, s32 vol, s32 pan, s32 pm, s32 pb,
                       const s8 *regs, int parent, u32 handle);

/* runs one grain; returns extra delay ticks */
static int do_grain_op(int hi)
{
    handler_t *h = &handlers[hi];
    bank_t *b = h->bank;
    int ng = sfx_ngrains(b, h->sfx);
    u8 *gp = (u8 *)grain_at(b, h->sfx, h->next_grain);
    u32 type = rd32(gp);
    s16 p0 = (s16)rd16(gp + 8), p1 = (s16)rd16(gp + 10), p2 = (s16)rd16(gp + 12);
    switch (type) {
    case 1: case 9: grain_tone(hi, gp); return 0;
    case 4: {                                    /* LFO settings */
        int which = gp[8] & 3;
        lfo_t *l = &h->lfo[which];
        l->target = gp[9];
        if (l->target != LT_NONE) {
            l->type = gp[11];
            l->target_extra = gp[10];
            l->setup_flags = rd16(gp + 16);
            l->depth = rd16(gp + 14);
            l->step_size = (s32)rd32(gp + 20);
            l->state_hold1 = 0;
            l->last_lfo = 0;
            if (l->type == LFO_SQUARE) l->state_hold1 = rd16(gp + 12);
            l->state_hold2 = 0;
            if (l->setup_flags & 2) l->next_step = (snd_rand() & 0x7FF) << 16;
            else l->next_step = rd16(gp + 18) << 16;
            if (l->type == LFO_RAND) { l->state_hold1 = -(snd_rand() & 0x7FFF) * (snd_rand() & 1); l->state_hold2 = 1; }
            lfo_calc_depth(hi, l);
            lfo_tick(hi, l);
        } else l->type = LFO_OFF;
        return 0;
    }
    case 5: {                                    /* start child sound */
        s32 vol = (s32)rd32(gp + 8), pan = (s32)rd32(gp + 12), id = (s32)rd32(gp + 20);
        if (vol < 0) vol = reg_value(h, vol, 0);
        vol = clampi(abs(vol), 0, 127);
        if (pan < 0) {
            if (pan >= -4) pan = 360 * (abs(h->regs[-pan - 1]) > 127 ? 127 : abs(h->regs[-pan - 1])) / 127;
            else if (pan == -5) pan = snd_rand() % 360;
            else { int r = abs(g_block_reg[(-pan - 6) & 31]); pan = 360 * (r > 127 ? 127 : r) / 127; }
        }
        if (id >= 0 && id < b->nsounds)
            handler_new(b, id, vol, pan, h->app_volume * h->orig_volume / 127, h->app_pan, h->app_pm, h->app_pb,
                        h->regs, hi, h->handle);
        return 0;
    }
    case 6: {                                    /* stop child sound */
        s32 id = (s32)rd32(gp + 20);
        if (id >= 0)
            for (int i = 0; i < NHANDLERS; i++)
                if (handlers[i].used && handlers[i].parent == hi && handlers[i].sfx == id) handler_free(i);
        return 0;
    }
    case 8: {                                    /* branch */
        s32 id = (s32)rd32(gp + 20);
        if (id < 0 || id >= b->nsounds) return 0;
        for (int i = 0; i < NVOICES; i++)
            if (voices[i].on && voices[i].handler == hi) { voice_key_off(&voices[i]); voices[i].voll = voices[i].volr = 0; }
        const u8 *e = sfx_entry(b, id);
        h->sfx = id;
        h->orig_volume = (s8)e[0];
        h->orig_pan = (s16)rd16(e + 2);
        h->cur_volume = (h->app_volume * (s8)e[0]) >> 10;
        h->group = (u8)e[1];
        h->countdown = sfx_ngrains(b, id) ? (s32)rd32(grain_at(b, id, 0) + 4) : 0;
        h->next_grain = -1;
        h->skip_grains = 0;
        h->grains_to_play = 0;
        h->grains_to_skip = 0;
        return 0;
    }
    case 21: case 20: case 35: case 0: return 0;   /* loop start, control null, marker, null */
    case 22: {                                   /* loop end: back to the loop start */
        for (int i = h->next_grain - 1; i >= 0; i--)
            if (rd32(grain_at(b, h->sfx, i)) == 21) { h->next_grain = i - 1; break; }
        return 0;
    }
    case 23: {                                   /* loop continue: past the loop end */
        for (int i = h->next_grain + 1; i < ng; i++)
            if (rd32(grain_at(b, h->sfx, i)) == 22) { h->next_grain = i; break; }
        return 0;
    }
    case 24: h->done = 1; return 0;              /* stop */
    case 25: {                                   /* random play: one of p0 groups of p1 grains */
        int options = p0, count = p1;
        if (options <= 0) return 0;
        int rnd = snd_rand() % options;
        if (rnd == p2) { rnd++; if (rnd >= options) rnd = 0; }
        gp[12] = (u8)rnd; gp[13] = (u8)(rnd >> 8);
        h->next_grain += rnd * count;
        h->grains_to_play = count + 1;
        h->grains_to_skip = (options - 1 - rnd) * count;
        h->skip_grains = 1;
        return 0;
    }
    case 26: {                                   /* random delay */
        s32 amount = (s32)rd32(gp + 8);
        return amount > 0 ? snd_rand() % amount : 0;
    }
    case 27: {                                   /* random pitch bend */
        s32 r = snd_rand();
        handler_set_pb(hi, p0 * ((0xFFFF * (r % 0x7FFF)) / 0x7FFF - 0x8000) / 100);
        return 0;
    }
    case 28:                                     /* pitch bend */
        handler_set_pb(hi, p0 >= 0 ? 0x7FFF * p0 / 127 : -0x8000 * p0 / -128);
        return 0;
    case 29:                                     /* add to the pitch bend */
        handler_set_pb(hi, clampi(h->cur_pb + 0x7FFF * p0 / 127, -32768, 32767));
        return 0;
    case 30:                                     /* set register */
        if (p0 < 0) g_block_reg[(-p0 - 1) & 31] = (s8)p1; else h->regs[p0 & 3] = (s8)p1;
        return 0;
    case 31: {                                   /* set register to a random value */
        s32 range = p2 - p1 + 1;
        s32 v = (range > 0 ? snd_rand() % range : 0) + p1;
        if (p0 < 0) g_block_reg[(-p0 - 1) & 31] = (s8)v; else h->regs[p0 & 3] = (s8)v;
        return 0;
    }
    case 32: case 33: {                          /* increment / decrement a register */
        s8 *r = p0 < 0 ? &g_block_reg[(-p0 - 1) & 31] : &h->regs[p0 & 3];
        *r = (s8)clampi(*r + (type == 32 ? 1 : -1), -128, 127);
        return 0;
    }
    case 34: {                                   /* test a register: skip the next grain */
        s32 v = p0 < 0 ? g_block_reg[(-p0 - 1) & 31] : h->regs[p0 & 3];
        if (p1 == 0) { if (v >= p2) h->next_grain++; }
        else if (p1 == 1) { if (v != p2) h->next_grain++; }
        else if (p2 >= v) h->next_grain++;
        return 0;
    }
    case 36: case 37: {                          /* go to a (random) marker */
        s32 mark = p0;
        if (type == 37) { s32 range = p1 - p0 + 1; mark = (range > 0 ? snd_rand() % range : 0) + p0; }
        for (int i = 0; i < ng; i++) {
            const u8 *g = grain_at(b, h->sfx, i);
            if (rd32(g) == 35 && (s16)rd16(g + 8) == mark) { h->next_grain = i - 1; break; }
        }
        return 0;
    }
    case 38:                                     /* wait for all voices */
        if (handler_has_voices(hi)) { h->next_grain--; return 1; }
        return 0;
    case 39: {                                   /* play cycle */
        int a = p2, idx = p2 + 1;
        if (idx == p0) idx = 0;
        gp[12] = (u8)idx; gp[13] = (u8)(idx >> 8);
        h->next_grain += p1 * a;
        h->grains_to_play = p1 + 1;
        h->grains_to_skip = (p0 - 1 - a) * p1;
        h->skip_grains = 1;
        return 0;
    }
    case 40: {                                   /* add to a register */
        s8 *r = p1 < 0 ? &g_block_reg[(-p1 - 1) & 31] : &h->regs[p1 & 3];
        *r = (s8)clampi(*r + p0, -128, 127);
        return 0;
    }
    case 41: case 42:                            /* key off / kill the voices */
        for (int i = 0; i < NVOICES; i++)
            if (voices[i].on && voices[i].handler == hi) {
                voice_key_off(&voices[i]);
                if (type == 42) voices[i].voll = voices[i].volr = 0;
            }
        return 0;
    case 43: h->next_grain = ng - 1; return 0;   /* on-stop marker */
    case 44: {                                   /* copy a register */
        s8 v = p0 < 0 ? g_block_reg[(-p0 - 1) & 31] : h->regs[p0 & 3];
        if (p1 < 0) g_block_reg[(-p1 - 1) & 31] = v; else h->regs[p1 & 3] = v;
        return 0;
    }
    case 7:                                      /* plugin message: none on RAC1's IOP */
    default:
        return 0;
    }
}

static void do_grain(int hi)
{
    handler_t *h = &handlers[hi];
    int ret = do_grain_op(hi);
    if (h->skip_grains) {
        h->grains_to_play--;
        if (h->grains_to_play == 0) { h->next_grain += h->grains_to_skip; h->skip_grains = 0; }
    }
    h->next_grain++;
    if (h->next_grain >= sfx_ngrains(h->bank, h->sfx) || h->next_grain < 0) { h->done = 1; return; }
    h->countdown = (s32)rd32(grain_at(h->bank, h->sfx, h->next_grain) + 4) + ret;
}

/* 989snd's instance limit: returns the handler to stop, -2 for "the new one", -1 for none */
static int check_instance_limit(int hi, s32 vol)
{
    handler_t *h = &handlers[hi];
    const u8 *e = sfx_entry(h->bank, h->sfx);
    int limit = (s8)e[5], flags = rd16(e + 6);
    if (!limit || !(flags & 8)) return -1;
    int weakest = -1, inst = 0;
    for (int i = 0; i < NHANDLERS; i++) {
        handler_t *o = &handlers[i];
        if (!o->used || i == hi || o->parent >= 0 || o->done) continue;
        if (o->bank == h->bank && (o->orig_sfx == h->sfx || o->sfx == h->sfx)) {
            inst++;
            if (weakest < 0 || ((flags & 0x10) && o->app_volume < handlers[weakest].app_volume) ||
                ((flags & 0x20) && o->start_tick < handlers[weakest].start_tick))
                weakest = i;
        }
    }
    if (limit - 1 < inst) {
        if (weakest >= 0 && (((flags & 0x10) && handlers[weakest].app_volume < vol) || (flags & 0x20))) return weakest;
        return -2;
    }
    return -1;
}

static int handler_new(bank_t *b, int sfx, s32 sfx_vol, s32 sfx_pan, s32 vol, s32 pan, s32 pm, s32 pb,
                       const s8 *regs, int parent, u32 handle)
{
    if (sfx < 0 || sfx >= b->nsounds || sfx_ngrains(b, sfx) <= 0) return -1;
    int hi;
    for (hi = 0; hi < NHANDLERS && handlers[hi].used; hi++) {}
    if (hi == NHANDLERS) { SLOG("989snd: out of sound handlers"); return -1; }
    const u8 *e = sfx_entry(b, sfx);
    handler_t *h = &handlers[hi];
    memset(h, 0, sizeof *h);
    h->used = 1;
    h->handle = handle;
    h->parent = parent;
    h->bank = b;
    h->sfx = h->orig_sfx = sfx;
    h->group = (u8)e[1];
    h->note = 60;
    h->fine = 0;
    h->start_tick = snd_tick;
    if (sfx_vol == -1) sfx_vol = (s8)e[0];
    if (sfx_pan == -1) sfx_pan = (s16)rd16(e + 2);
    if (vol == VOLUME_DONT_CHANGE) vol = 1024;
    s32 play_vol = (sfx_vol * vol) >> 10;
    if (play_vol >= 128) play_vol = 127;
    if (pan == PAN_RESET || pan == PAN_DONT_CHANGE) pan = sfx_pan;
    h->orig_volume = sfx_vol;
    h->orig_pan = sfx_pan;
    h->cur_volume = play_vol;
    h->cur_pan = pan;
    h->cur_pb = pb;
    h->cur_pm = pm;
    h->app_volume = vol;
    h->app_pan = pan;
    h->app_pb = pb;
    h->app_pm = pm;
    if (regs) memcpy(h->regs, regs, 4);
    if (parent < 0) {
        int stop = check_instance_limit(hi, vol);
        if (stop == -2) { h->used = 0; return -1; }
        if (stop >= 0) handler_stop(stop);
    }
    h->next_grain = 0;
    h->countdown = (s32)rd32(grain_at(b, sfx, 0) + 4);
    while (h->countdown <= 0 && !h->done) do_grain(hi);
    return hi;
}

/* one 240 Hz tick of a handler; returns 1 when it is finished */
static int handler_tick(int hi)
{
    handler_t *h = &handlers[hi];
    for (int i = 0; i < 4; i++) lfo_tick(hi, &h->lfo[i]);
    if (h->done && !handler_has_children(hi)) return !handler_has_voices(hi);
    if (h->paused || h->done) return 0;
    h->countdown--;
    while (h->countdown <= 0 && !h->done) do_grain(hi);
    return 0;
}

static int find_handler(u32 handle)
{
    if (!handle) return -1;
    for (int i = 0; i < NHANDLERS; i++)
        if (handlers[i].used && handlers[i].parent < 0 && handlers[i].handle == handle) return i;
    return -1;
}

/* ===================================================================== */
/* VAG streams                                                            */
/* ===================================================================== */

typedef struct stream {
    int used;
    u32 handle;
    u32 lsn;                         /* the VAGp file on the disc */
    u32 size;                        /* data bytes after the 0x30-byte header */
    u32 rate;
    u8 *buf;                         /* the file's data, read as it plays */
    u32 loaded;                      /* bytes of buf read so far */
    s32 vol, pan, group, flags;
    int paused;
    int voice;                       /* index in voices[] */
    struct { int on; u32 lsn; s32 vol, pan, flags; } next;   /* queued after this one */
} stream_t;

#define NSTREAMS 8
#define STREAM_CHUNK 32              /* sectors read at a time */
static stream_t streams[NSTREAMS];

static int stream_open_file(stream_t *s, u32 lsn)
{
    u8 head[2048];
    if (uya_disc_read(lsn, 1, head) != 0 || memcmp(head, "VAGp", 4)) {
        uya_log("989snd: no VAG stream at LSN %u", lsn);
        return 0;
    }
    u32 size = rd32be(head + 12), rate = rd32be(head + 16);
    if (size == 0 || size > (256u << 20)) return 0;
    free(s->buf);
    u32 total = 0x30 + size;
    s->buf = malloc(((total + 2047) / 2048 + 1) * 2048);
    if (!s->buf) return 0;
    memcpy(s->buf, head, 2048);
    s->loaded = 2048;
    s->lsn = lsn;
    s->size = size;
    s->rate = rate ? rate : 44100;
    return 1;
}

/* the 16-byte ADPCM block at data offset off, reading the disc as needed */
static const u8 *stream_block(stream_t *s, u32 off)
{
    if (!s->buf || off + 16 > s->size) return 0;
    u32 need = 0x30 + off + 16;
    while (s->loaded < need) {
        u32 sec = s->loaded / 2048, total = (0x30 + s->size + 2047) / 2048, n = total - sec;
        if (n > STREAM_CHUNK) n = STREAM_CHUNK;
        if (n == 0 || uya_disc_read(s->lsn + sec, n, s->buf + s->loaded) != 0) return 0;
        s->loaded += n * 2048;
    }
    return s->buf + 0x30 + off;
}

static void stream_set_volume(stream_t *s)
{
    voice_t *v = &voices[s->voice];
    s32 vol = clampi(s->vol, 0, 0x400) * 127 / 0x400;
    make_volume(127, 0, vol, s->pan, 127, 0, &v->base_l, &v->base_r);
    v->group = (u8)s->group;
    voice_apply_volume(v);
}

static void stream_free(stream_t *s)
{
    if (s->voice >= 0 && voices[s->voice].stream == s) { voices[s->voice].on = 0; voices[s->voice].stream = 0; }
    free(s->buf);
    memset(s, 0, sizeof *s);
    s->voice = -1;
}

/* the voice reached the end of the stream's data */
static void stream_voice_end(voice_t *v)
{
    stream_t *s = v->stream;
    if (s->next.on) {
        u32 lsn = s->next.lsn;
        s->vol = s->next.vol; s->pan = s->next.pan; s->flags = s->next.flags;
        s->next.on = 0;
        if (stream_open_file(s, lsn)) {
            SLOG("989snd: stream %08X continues with LSN %u", s->handle, lsn);
            v->nax = 0; v->first = 1; v->sub = 28; v->flags = 0;
            v->pitch = (u16)((u64)s->rate * 4096 / 48000);
            stream_set_volume(s);
            return;
        }
    }
    if (s->flags & 4) { v->nax = 0; v->flags = 0; return; }   /* looped */
    voice_kill(v);
}

static stream_t *find_stream(u32 handle)
{
    if (!handle) return 0;
    for (int i = 0; i < NSTREAMS; i++) if (streams[i].used && streams[i].handle == handle) return &streams[i];
    return 0;
}

static u32 stream_play(const s32 *a)
{
    u32 lsn = (u32)a[0], queue = (u32)a[5];
    s32 vol = a[2] >> 16, pan = a[3] >> 16, group = a[4], flags = a[6];
    SLOG("989snd: VAG stream LSN %u (LSN2 %u) vol %d pan %d group %d queue %08X flags %X", lsn, (u32)a[1], vol, pan, group, queue, flags);
    stream_t *q = find_stream(queue);
    if (q) {                                     /* plays when that one ends, same handle */
        q->next.on = 1; q->next.lsn = lsn; q->next.vol = vol; q->next.pan = pan; q->next.flags = flags;
        return q->handle;
    }
    int si, vi;
    for (si = 0; si < NSTREAMS && streams[si].used; si++) {}
    if (si == NSTREAMS) return 0;
    for (vi = NVOICES - 1; vi >= 0 && voices[vi].on; vi--) {}
    if (vi < 0) {
        voice_t *v = voice_alloc(127);
        if (!v) return 0;
        vi = (int)(v - voices);
    }
    stream_t *s = &streams[si];
    memset(s, 0, sizeof *s);
    s->voice = -1;
    if (!stream_open_file(s, lsn)) { free(s->buf); s->buf = 0; return 0; }
    s->used = 1;
    s->handle = new_handle();
    s->vol = vol; s->pan = pan; s->group = group; s->flags = flags;
    s->voice = vi;
    voice_t *v = &voices[vi];
    memset(v, 0, sizeof *v);
    v->handler = -1;
    v->stream = s;
    v->size = s->size;
    v->adsr1 = 0x00FF;                           /* attack at once, held */
    v->adsr2 = 0x1FC0;
    v->pitch = (u16)((u64)s->rate * 4096 / 48000);
    voice_start(v);
    v->level = 0x7FFF;
    v->phase = PH_SUSTAIN;
    adsr_settings(v);
    stream_set_volume(s);
    s->paused = v->paused = (flags & 1) != 0;   /* started paused: continued by command 0x2E */
    return s->handle;
}

static int stream_active(stream_t *s) { return s && s->voice >= 0 && voices[s->voice].on && voices[s->voice].stream == s; }

/* ===================================================================== */
/* the mixer                                                              */
/* ===================================================================== */

static int htick = 200;

static void tick_all(void)
{
    snd_tick++;
    for (int i = 0; i < NHANDLERS; i++) {
        if (!handlers[i].used) continue;
        if (handler_tick(i)) handler_free(i);
    }
    for (int i = 0; i < NSTREAMS; i++)
        if (streams[i].used && !stream_active(&streams[i])) {
            SLOG("989snd: stream %08X ended", streams[i].handle);
            stream_free(&streams[i]);
        }
}

/* ---- the SPU2's reverb ----
 * The hardware's effect: a work area of 16-bit samples running at half the
 * output rate, with same-side and cross reflections, four comb taps and two
 * all-pass filters (the PS1/PS2 formula as documented in nocash's psx-spx).
 * The presets are libsd's effect modes (sceSdSetEffectAttr: room, studio
 * A/B/C, hall, space, echo, delay, pipe), whose register values are the PS1
 * library's: address registers in 8-byte units, sizes in bytes. For echo and
 * delay the delay and feedback parameters recompute some of them. Voices whose
 * tone has flag 1 (989snd's "reverb" output) feed it; 989snd command 0x50 sets
 * the mode, depth (the return volume, 0-0x7FFF), delay and feedback, and 0x10
 * fades the depth. */
typedef struct {
    u32 size;
    s16 dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL;
    s16 vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
    s16 dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
    s16 dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN;
} rev_preset_t;

#define H(x) ((s16)(x))
static const rev_preset_t rev_presets[10] = {
    {0},                                                                                       /* off */
    {0x26C0, 0x007D, 0x005B, 0x6D80, 0x54B8, H(0xBED0), 0x0000, 0x0000, H(0xBA80),           /* room */
     0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
     0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, H(0x8000), H(0x8000)},
    {0x1F40, 0x0033, 0x0025, 0x70F0, 0x4FA8, H(0xBCE0), 0x4410, H(0xC0F0), H(0x9C00),        /* studio A (small) */
     0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
     0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
     0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, H(0x8000), H(0x8000)},
    {0x4840, 0x00B1, 0x007F, 0x70F0, 0x4FA8, H(0xBCE0), 0x4510, H(0xBEF0), H(0xB4C0),        /* studio B (medium) */
     0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
     0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
     0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, H(0x8000), H(0x8000)},
    {0x6FE0, 0x00E3, 0x00A9, 0x6F60, 0x4FA8, H(0xBCE0), 0x4510, H(0xBEF0), H(0xA680),        /* studio C (large) */
     0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
     0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
     0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, H(0x8000), H(0x8000)},
    {0xADE0, 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, H(0xB800), H(0xBC00), H(0xC000),        /* hall */
     0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
     0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
     0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, H(0x8000), H(0x8000)},
    {0xF6C0, 0x033D, 0x0231, 0x7E00, 0x5000, H(0xB400), H(0xB000), 0x4C00, H(0xB000),        /* space */
     0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
     0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
     0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, H(0x8000), H(0x8000)},
    {0x18040, 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, H(0x8100),             /* echo */
     0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
     0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, H(0x8000), H(0x8000)},
    {0x18040, 0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,                /* delay */
     0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
     0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, H(0x8000), H(0x8000)},
    {0x3C00, 0x0017, 0x0013, 0x70F0, 0x4FA8, H(0xBCE0), 0x4510, H(0xBEF0), H(0x8500),        /* pipe (half echo) */
     0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
     0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
     0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, H(0x8000), H(0x8000)},
};
#undef H

static struct {
    int mode;
    rev_preset_t p;
    s16 *buf;
    u32 n, pos;                      /* work area size and position, in samples */
    s32 depth, depth_target, depth_step;
    s32 inl, inr, half;              /* input summed over two output samples */
    s32 outl, outr, prevl, prevr;    /* last two outputs, for the upsampling */
} rev;

static void reverb_set(s32 mode, s32 depth, s32 delay, s32 feedback)
{
    if (mode < 0 || mode > 9) mode = 0;
    rev.depth = rev.depth_target = clampi(depth, -0x8000, 0x7FFF);
    rev.depth_step = 0;
    if (mode != rev.mode) {
        rev.mode = mode;
        rev.p = rev_presets[mode];
        free(rev.buf);
        rev.buf = 0;
        rev.n = rev.pos = 0;
        if (mode) { rev.n = rev.p.size / 2; rev.buf = calloc(rev.n, 2); if (!rev.buf) rev.mode = 0; }
        rev.outl = rev.outr = rev.prevl = rev.prevr = 0;
    }
    if (mode == 7 || mode == 8) {    /* echo, delay: delay and feedback set some registers */
        rev_preset_t *p = &rev.p;
        s32 d16 = (clampi(delay, 0, 127) + 1) * 16;
        *p = rev_presets[mode];
        p->mLSAME = (s16)(d16 * 2 - p->dAPF1);
        p->mRSAME = (s16)(d16 - p->dAPF2);
        p->mLCOMB1 = (s16)(p->mRCOMB1 + d16);
        p->dLSAME = (s16)(p->dRSAME + d16);
        p->mLAPF1 = (s16)(p->mLAPF2 + d16);
        p->mRAPF1 = (s16)(d16 + (p->mLAPF2 >> 1));
        p->vWALL = (s16)(-(clampi(feedback, 0, 127) << 8));
    }
}

static inline s16 *rv(s32 reg, s32 back)                /* the sample at register offset reg (8-byte units) */
{
    s32 a = (s32)rev.pos + reg * 4 - back;
    a %= (s32)rev.n;
    if (a < 0) a += (s32)rev.n;
    return &rev.buf[a];
}
static inline s16 sat16(s32 v) { return (s16)clampi(v, -32768, 32767); }
static inline s32 mul15(s32 a, s32 b) { return (a * b) >> 15; }

/* one reverb step (at half the output rate) */
static void reverb_step(s32 lin16, s32 rin16)
{
    const rev_preset_t *p = &rev.p;
    s32 Lin = mul15(p->vLIN, lin16), Rin = mul15(p->vRIN, rin16);
    s16 *t;
    t = rv(p->mLSAME, 1); *rv(p->mLSAME, 0) = sat16(mul15(Lin + mul15(*rv(p->dLSAME, 0), p->vWALL) - *t, p->vIIR) + *t);
    t = rv(p->mRSAME, 1); *rv(p->mRSAME, 0) = sat16(mul15(Rin + mul15(*rv(p->dRSAME, 0), p->vWALL) - *t, p->vIIR) + *t);
    t = rv(p->mLDIFF, 1); *rv(p->mLDIFF, 0) = sat16(mul15(Lin + mul15(*rv(p->dRDIFF, 0), p->vWALL) - *t, p->vIIR) + *t);
    t = rv(p->mRDIFF, 1); *rv(p->mRDIFF, 0) = sat16(mul15(Rin + mul15(*rv(p->dLDIFF, 0), p->vWALL) - *t, p->vIIR) + *t);
    s32 L = mul15(p->vCOMB1, *rv(p->mLCOMB1, 0)) + mul15(p->vCOMB2, *rv(p->mLCOMB2, 0)) +
            mul15(p->vCOMB3, *rv(p->mLCOMB3, 0)) + mul15(p->vCOMB4, *rv(p->mLCOMB4, 0));
    s32 R = mul15(p->vCOMB1, *rv(p->mRCOMB1, 0)) + mul15(p->vCOMB2, *rv(p->mRCOMB2, 0)) +
            mul15(p->vCOMB3, *rv(p->mRCOMB3, 0)) + mul15(p->vCOMB4, *rv(p->mRCOMB4, 0));
    s32 x;
    x = *rv(p->mLAPF1 - p->dAPF1, 0); L = sat16(L - mul15(p->vAPF1, x)); *rv(p->mLAPF1, 0) = (s16)L; L = mul15(L, p->vAPF1) + x;
    x = *rv(p->mRAPF1 - p->dAPF1, 0); R = sat16(R - mul15(p->vAPF1, x)); *rv(p->mRAPF1, 0) = (s16)R; R = mul15(R, p->vAPF1) + x;
    x = *rv(p->mLAPF2 - p->dAPF2, 0); L = sat16(L - mul15(p->vAPF2, x)); *rv(p->mLAPF2, 0) = (s16)L; L = mul15(L, p->vAPF2) + x;
    x = *rv(p->mRAPF2 - p->dAPF2, 0); R = sat16(R - mul15(p->vAPF2, x)); *rv(p->mRAPF2, 0) = (s16)R; R = mul15(R, p->vAPF2) + x;
    rev.prevl = rev.outl; rev.prevr = rev.outr;
    rev.outl = mul15(sat16(L), rev.depth);
    rev.outr = mul15(sat16(R), rev.depth);
    if (++rev.pos >= rev.n) rev.pos = 0;
}

/* feeds one output sample's wet input; adds the reverb's return to l, r */
static void reverb_run(s32 wl, s32 wr, s32 *l, s32 *r)
{
    if (!rev.mode || !rev.buf) return;
    rev.inl += wl; rev.inr += wr;
    if (rev.half) {
        reverb_step(sat16(rev.inl >> 1), sat16(rev.inr >> 1));
        rev.inl = rev.inr = 0;
        *l += (rev.prevl + rev.outl) >> 1;           /* between the last two outputs */
        *r += (rev.prevr + rev.outr) >> 1;
    } else {
        *l += rev.outl;
        *r += rev.outr;
    }
    rev.half ^= 1;
}

static void snd_mix(s16 *out, int frames)
{
    for (int f = 0; f < frames; f++) {
        if (++htick >= 200) {
            htick = 0;
            tick_all();
            if (rev.depth_step) {                    /* snd_AutoReverb's fade */
                rev.depth += rev.depth_step;
                if ((rev.depth_step > 0 && rev.depth >= rev.depth_target) || (rev.depth_step < 0 && rev.depth <= rev.depth_target)) {
                    rev.depth = rev.depth_target;
                    rev.depth_step = 0;
                }
            }
        }
        s32 l = 0, r = 0, wl = 0, wr = 0;
        for (int i = 0; i < NVOICES; i++) {
            voice_t *v = &voices[i];
            if (!v->on) continue;
            if (v->phase == PH_STOPPED) {
                v->on = 0;
                continue;
            }
            s32 vl = 0, vr = 0;
            voice_run(v, &vl, &vr);
            l += vl;
            r += vr;
            if (!v->stream && (v->tone.flags & 1)) { wl += vl; wr += vr; }
        }
        reverb_run(wl, wr, &l, &r);
        out[f * 2] = (s16)clampi(l, -32768, 32767);
        out[f * 2 + 1] = (s16)clampi(r, -32768, 32767);
    }
}

/* Called once per vsync (host/src/sdk/graph.c). The sound runs on the game's
 * clock, as the PS2's SPU2 and EE share one: every vsync mixes exactly one
 * field's worth of sound (960 frames at 50 Hz), so a VAG stream (a cut-scene's
 * dialog and foley) and the sounds the game starts stay where the game is,
 * whatever the speed of the machine. (Mixing as the output device consumed
 * sound let the sound run on in real time while the game was held up: after
 * every slow frame the mixer caught up by up to 200 ms at once, so the
 * cut-scene streams ran ahead of the pictures.)
 * The output keeps a small cushion: when it has (nearly) run dry (the start,
 * a movie, a frame the machine was too slow for) silence is put in first; when
 * it holds far too much (the output's clock is a little slower than the
 * game's), a field's sound is dropped. */
uint32_t uya_frame_us(void);
#define OUT_TARGET 3840                          /* 80 ms of cushion */
#define OUT_LOW 960                              /* below: refill with silence */
#define OUT_HIGH 14400                           /* above (300 ms): drop */
void uya_sound_pump(void)
{
    static s16 buf[1024 * 2], zero[1024 * 2];
    static u32 mixed;
    static u64 due_us;                           /* game time not mixed yet (us * 48000) */
    if (snd_log > 0) {                           /* the music player's stream records (music.c, D_001517D0): their states */
        static s16 last[3] = {-1, -1, -1};
        static const char *name[3] = {"music", "dialog", "transition"};
        for (int i = 0; i < 3; i++) {
            s16 st = *(volatile s16 *)(uintptr_t)(0x001517D0 + 0x34 + 0x1C * i + 0x0A);
            if (st != last[i]) { uya_log("v%u 989snd: %s stream record state %d -> %d", uya_frame_count(), name[i], last[i], st); last[i] = st; }
        }
    }
    due_us += (u64)uya_frame_us() * 48000u;
    int n = (int)(due_us / 1000000u);
    due_us -= (u64)n * 1000000u;
    if (n > 1024) n = 1024;
    snd_mix(buf, n);
    int q = plat_stream_queued();
    if (q < OUT_LOW)
        for (int k = OUT_TARGET - q; k > 0; k -= 1024) plat_stream_write(zero, k > 1024 ? 1024 : k);
    if (q <= OUT_HIGH) plat_stream_write(buf, n);
    u32 before = mixed;
    mixed += (u32)n;
    if (snd_log > 0 && mixed / (48000 * 5) != before / (48000 * 5)) {          /* a status line every 5 s of sound */
        int nv = 0, nh = 0;
        for (int i = 0; i < NVOICES; i++) nv += voices[i].on;
        for (int i = 0; i < NHANDLERS; i++) nh += handlers[i].used && handlers[i].parent < 0;
        uya_log("v%u 989snd: %u s of sound: %d voices, %d sounds playing, %d frames queued", uya_frame_count(), mixed / 48000, nv, nh, q);
        for (int i = 0; i < NSTREAMS; i++) {
            stream_t *s = &streams[i];
            if (!s->used) continue;
            uya_log("989snd:   stream %08X LSN %u at %u/%u%s, vol %d group %d", s->handle, s->lsn,
                    s->voice >= 0 ? voices[s->voice].nax : 0, s->size, s->paused ? " (paused)" : "", s->vol, s->group);
        }
    }
}

/* ===================================================================== */
/* the command protocol                                                   */
/* ===================================================================== */

/* the EE's stream-safe CD state (snd_StreamSafeCdRead and friends, 989snd.c) */
#define CDSAFE_COUNT 0x00137C00
#define CDSAFE_ERROR 0x00137C10

static void init_once(void)
{
    if (snd_log >= 0) return;
    snd_log = getenv("UYA_SNDLOG") != 0;
    for (int i = 0; i < 32; i++) master_vol[i] = 0x400;
    for (int i = 0; i < NSTREAMS; i++) streams[i].voice = -1;
    for (int i = 0; i < NVOICES; i++) voices[i].handler = -1;
}

/* testing: UYA_SNDTRACE=n logs the EE return addresses found on the
 * translated code's stack (a heuristic backtrace) when sound n is played */
static void snd_trace(int sfx)
{
    static int want = -2;
    if (want == -2) { const char *e = getenv("UYA_SNDTRACE"); want = e ? atoi(e) : -1; }
    if (want < 0 || sfx != want) return;
    const char *rc_stack_scan(void);
    uya_log("v%u sndtrace %d:%s", uya_frame_count(), sfx, rc_stack_scan());
}

static s32 play_sound(const s32 *a)
{
    snd_trace(a[1]);
    bank_t *b = bank_get((u32)a[0]);
    if (!b) { SLOG("989snd: play: no bank %08X", (u32)a[0]); return 0; }
    u32 h = new_handle();
    int hi = handler_new(b, a[1], -1, -1, a[2], a[3], a[4], a[5], 0, -1, h);
    SLOG("989snd: play bank %08X sound %d vol %d pan %d pm %d pb %d -> %08X", (u32)a[0], a[1], a[2], a[3], a[4], a[5], hi >= 0 ? h : 0);
    if (hi < 0) return 0;
    if (handlers[hi].done && !handler_has_voices(hi) && !handler_has_children(hi)) { handler_free(hi); return 0; }
    return (s32)h;
}

static int still_playing(u32 h)
{
    if (find_handler(h) >= 0) return 1;
    return stream_active(find_stream(h));
}

s32 rac1_snd_command(s32 cmd, s32 size, const void *data)
{
    const s32 *a = (const s32 *)data;
    init_once();
    if (snd_log && cmd != 0x09 && cmd != 0x19 && cmd != 0x32 && cmd != 0x4F && cmd != 0x21 && cmd != 0x15 && cmd != 0x11) {
        char t[128] = "";
        int n = 0;
        for (int i = 0; i < size / 4 && i < 8; i++) n += snprintf(t + n, sizeof t - n, " %X", (u32)a[i]);
        uya_log("v%u 989snd command 0x%02X:%s", uya_frame_count(), cmd, t);
    }
    switch (cmd) {
    case 0x06: {                                 /* snd_UnloadBank */
        bank_t *b = bank_get((u32)a[0]);
        if (!b) return 0;
        for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].bank == b) handler_free(i);
        free(b->data);
        memset(b, 0, sizeof *b);
        return 0;
    }
    case 0x08: return 0;                         /* snd_ResolveBankXREFS */
    case 0x09: {                                 /* snd_SetMasterVolume(group, vol) */
        s32 g = a[0], vol = clampi(a[1], 0, 0x400);
        if (g < 0 || g >= 32 || g == 15) return 0;
        master_vol[g] = vol;
        for (int i = 0; i < NVOICES; i++) if (voices[i].on && voices[i].group == g && !voices[i].paused) voice_apply_volume(&voices[i]);
        return 0;
    }
    case 0x0B: stereo_mode = a[0]; return 0;     /* snd_SetPlaybackMode */
    case 0x0D: return 0;                         /* mixer mode */
    case 0x10: {                                 /* snd_AutoReverb(core, depth, ticks, channels): fades the depth */
        s32 target = clampi(a[1], -0x8000, 0x7FFF), ticks = a[2] > 0 ? a[2] : 1;
        rev.depth_target = target;
        rev.depth_step = (target - rev.depth) / ticks;
        if (rev.depth_step == 0) { rev.depth = target; }
        return 0;
    }
    case 0x11: return play_sound(a);             /* snd_PlaySoundVolPanPMPB */
    case 0x15: {                                 /* snd_StopSound */
        int hi = find_handler((u32)a[0]);
        if (hi >= 0 && !handlers[hi].done) SLOG("989snd: stop %08X (bank %08X sound %d)", (u32)a[0], handlers[hi].bank->handle, handlers[hi].sfx);
        if (hi >= 0) handler_stop(hi);
        stream_t *s = find_stream((u32)a[0]);
        if (s) { s->next.on = 0; if (stream_active(s)) voice_kill(&voices[s->voice]); }
        return 0;
    }
    case 0x16: case 0x17: {                      /* snd_PauseAllSoundsInGroup / Continue (group mask) */
        u32 mask = (u32)a[0];
        int pause = cmd == 0x16;
        for (int i = 0; i < NHANDLERS; i++)
            if (handlers[i].used && handlers[i].parent < 0 && (mask & (1u << (handlers[i].group & 31)))) handler_pause(i, pause);
        for (int i = 0; i < NSTREAMS; i++) {
            stream_t *s = &streams[i];
            if (!stream_active(s) || !(mask & (1u << (s->group & 31)))) continue;
            voice_t *v = &voices[s->voice];
            if (pause) v->paused = 1;
            else { v->paused = s->paused; stream_set_volume(s); }
        }
        return 0;
    }
    case 0x18:                                   /* snd_StopAllSounds */
        for (int i = 0; i < NHANDLERS; i++) if (handlers[i].used && handlers[i].parent < 0) handler_stop(i);
        return 0;
    case 0x19:                                   /* snd_SoundIsStillPlaying */
        return still_playing((u32)a[0]) ? a[0] : 0;
    case 0x21: {                                 /* snd_SetSoundParams(handle, mask, vol, pan, pm, pb) */
        u32 h = (u32)a[0];
        s32 mask = a[1];
        int hi = find_handler(h);
        if (hi >= 0) {
            if (mask & 3) handler_set_vol_pan(hi, (mask & 1) ? a[2] : VOLUME_DONT_CHANGE, (mask & 2) ? a[3] : PAN_DONT_CHANGE);
            if (mask & 4) handler_set_pm(hi, a[4]);
            if (mask & 8) handler_set_pb(hi, a[5]);
            return (s32)h;
        }
        stream_t *s = find_stream(h);
        if (stream_active(s)) {
            if (mask & 1) s->vol = a[2];
            if (mask & 2) s->pan = a[3];
            stream_set_volume(s);
            return (s32)h;
        }
        return 0;
    }
    case 0x2A: return 1;                         /* snd_InitVAGStreamingEx: streaming is on */
    case 0x2C: return (s32)stream_play(a);       /* snd_PlayVAGStreamByLocEx */
    case 0x2D: case 0x2E: {                      /* snd_PauseVAGStream / snd_ContinueVAGStream */
        stream_t *s = find_stream((u32)a[0]);
        if (!stream_active(s)) return 0;
        s->paused = voices[s->voice].paused = cmd == 0x2D;
        if (cmd == 0x2E) stream_set_volume(s);
        return 0;
    }
    case 0x32: {                                 /* snd_GetVAGStreamTimeRemaining (ms) */
        stream_t *s = find_stream((u32)a[0]);
        if (!stream_active(s)) return 0;
        voice_t *v = &voices[s->voice];
        u32 left = s->size > v->nax ? (s->size - v->nax) / 16 * 28 : 0;
        return (s32)((u64)left * 1000 / s->rate);
    }
    case 0x34:                                   /* snd_StopAllStreams */
        for (int i = 0; i < NSTREAMS; i++) if (streams[i].used) stream_free(&streams[i]);
        return 0;
    case 0x36: return 0;                         /* snd_StreamSafeCheckCDIdle */
    case 0x37:                                   /* snd_StreamSafeCdBreak */
        EEW(CDSAFE_COUNT) = 0;
        return 0;
    case 0x38: {                                 /* snd_StreamSafeCdRead(lsn, sectors, ee address) */
        int err = uya_disc_read((u32)a[0], (u32)a[1], EE(a[2])) != 0;
        if (err) uya_log("989snd: stream-safe read of LSN %u (+%u) failed", (u32)a[0], (u32)a[1]);
        EEW(CDSAFE_ERROR) = err ? 0x32 : 0;
        EEW(CDSAFE_COUNT) = 0;
        return 0;
    }
    case 0x4E: return 0;                         /* snd_SetGroupVoiceRange */
    case 0x4F: {                                 /* snd_IsVAGStreamBuffered */
        stream_t *s = find_stream((u32)a[0]);
        return stream_active(s) ? 1 : 0;
    }
    case 0x50:                                   /* snd_SetReverbEx(core, mode, depth, delay, feedback) */
        reverb_set(a[1], a[2], a[3], a[4]);
        return 0;
    case 0x51: return 0;                         /* snd_PreAllocReverbWorkArea */
    default:
        SLOG("989snd: command 0x%02X not handled", cmd);
        return 0;
    }
}

/* snd_BankLoadByLoc (0x0012E060) */
u32 rac1_snd_bank_load_lsn(s32 lsn, s32 off)
{
    u8 head[2048];
    init_once();
    if (uya_disc_read((u32)lsn, 1, head) != 0) return 0;
    u32 size = bank_file_size(head + (off & 2047));
    if (size < 0x18 || size > (32u << 20)) { uya_log("989snd: no bank at LSN %d", lsn); return 0; }
    u32 sectors = ((off & 2047) + size + 2047) / 2048;
    u8 *b = malloc((size_t)sectors * 2048);
    u32 h = 0;
    if (b && uya_disc_read((u32)lsn + (u32)off / 2048, sectors, b) == 0) h = bank_add(b + (off & 2047), size);
    free(b);
    return h;
}

/* snd_BankLoadFromEE (0x0012E1C8, command 0x57) */
u32 rac1_snd_bank_load_ee(u32 addr)
{
    init_once();
    const u8 *src = EE(addr);
    return bank_add(src, bank_file_size(src));
}

/* ---- replies owed to callbacks, delivered at the next flush ---- */
typedef void (*snd_cb_t)(s32 result, s64 arg);
static struct { snd_cb_t fn; s64 arg; s32 result; } owed[256];
static int nowed;

void rac1_snd_owe(void *fn, s64 arg, s32 result)
{
    if (!fn) return;
    if (nowed < 256) { owed[nowed].fn = (snd_cb_t)fn; owed[nowed].arg = arg; owed[nowed].result = result; nowed++; }
    else uya_log("989snd: too many replies owed");
}

void rac1_snd_deliver(void)
{
    static struct { snd_cb_t fn; s64 arg; s32 result; } now[256];
    int n = nowed;
    memcpy(now, owed, sizeof now[0] * (size_t)n);
    nowed = 0;                                   /* callbacks may send commands: those wait for the next flush */
    for (int i = 0; i < n; i++) now[i].fn(now[i].result, now[i].arg);
}

/* snd_FlushSoundCommands (0x0012DDC0), host version: the IOP's replies are
 * in, so the callbacks run; then, as the library does, a finished bank load
 * from EE memory (command 0x57) gets its callback, and a finished
 * stream-safe CD read gets the CD callback (snd_StreamSafeCdCallback's).
 * Returns whether anything is still in flight: nothing is. */
extern s32 func_0012EF48(s32);                   /* snd_StreamSafeCdSync */
#define SDA(a) (*(volatile s32 *)(uintptr_t)(a))
s32 rac1_snd_flush(void)
{
    static int depth;
    if (depth > 4) return 0;
    depth++;
    rac1_snd_deliver();
    if (SDA(0x0015EDC8) != 0 && SDA(0x0015EE00) != -1) {          /* bank load from EE done */
        snd_cb_t fn = (snd_cb_t)(uintptr_t)(u32)SDA(0x0015EDD0);
        s64 arg = *(volatile s64 *)(uintptr_t)0x0015EDD8;
        s32 r = SDA(0x0015EE00);
        SDA(0x0015EDD0) = 0;
        *(volatile s64 *)(uintptr_t)0x0015EDD8 = 0;
        SDA(0x0015EE00) = 0;
        SDA(0x0015EDC8) = 0;
        if (fn) fn(r, arg);
    }
    if (SDA(0x0015ED94) != 0) {                                    /* stream-safe CD read requested */
        func_0012EF48(1);
        if (SDA(0x0015ED98) != 0) {
            void (*cb)(s32) = (void (*)(s32))(uintptr_t)(u32)SDA(0x0015ED90);
            SDA(0x0015ED94) = 0;
            SDA(0x0015ED98) = 0;
            if (cb) cb(1);
        }
    }
    depth--;
    return 0;
}
