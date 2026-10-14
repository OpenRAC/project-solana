/* 0x0022DBE8: SoundMasterVolumeInit (sound.c, still assembly in the decomp).
 * Ported from the assembly: clears the sound tables at 0x0013E650, starts
 * 989snd, sets playback mode, reverb and voice ranges, derives the six master
 * volumes from the option values at 0x0015EEEC/0x0015EEF0, starts the music
 * streamer (func_002161E0, which also registers the CD callback the
 * streaming loads wait on) and sets the volumes. */
#include "common.h"

extern void func_0012DB68(void);                 /* snd_StartSoundSystem */
extern void func_0012E380(s32);                  /* snd_SetPlaybackMode */
extern void func_0012E3C0(s32, s32);
extern void func_0012F0E8(s32, s32);             /* snd_PreAllocReverbWorkArea */
extern void func_0012E3F8(s32, s32, s32);        /* snd_SetGroupVoiceRange */
extern void func_0012E348(s32, s32);             /* snd_SetMasterVolume */
extern void func_002161E0(void);                 /* MusicStreamInit */

void func_0022DBE8(void)
{
    u8 *b = (u8 *)0x0013E650;
    s32 vol, v8, v7;
    __builtin_memset(b, 0, 0x40);
    *(u32 *)(b + 0x40) = 0;
    *(u32 *)(b + 0x44) = 0;
    for (s32 k = 1; k <= 30; k++) { *(u32 *)(b + 0x70 * k) = 0; b[0x70 * k + 4] = 0; }
    func_0012DB68();
    func_0012E380(*(s32 *)0x0015EEE8 == 0);
    func_0012E3C0(0, 1);
    func_0012F0E8(2, 4);
    func_0012E3F8(1, 0x18, 0x2F);
    func_0012E3F8(2, 0x18, 0x2F);
    func_0012E3F8(4, 0x18, 0x2F);
    vol = *(s32 *)0x0015EEF0;
    v8 = vol * 8 / 10;
    v7 = vol * 7 / 10;
    *(s32 *)(b + 0x4C) = *(s32 *)0x0015EEEC;
    *(s32 *)(b + 0x48) = v8;
    *(s32 *)(b + 0x50) = v8;
    *(s32 *)(b + 0x54) = v7;
    *(s32 *)(b + 0x58) = v7;
    *(s32 *)(b + 0x5C) = vol;
    func_002161E0();
    for (s32 i = 0; i < 6; i++) func_0012E348(i, *(s32 *)(b + 0x48 + 4 * i));
}
