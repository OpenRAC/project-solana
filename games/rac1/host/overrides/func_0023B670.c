/* 0x0023B670: play a movie (src/game/movie/movie.c): (sector, size, buffer,
 * player state, language). The Sony sample player it runs (IPU, threads,
 * SPU, uncached buffers) is replaced by the host's MPEG-2 player
 * (host/src/movie/player.c); the language picks the movie's sound track. */
#include "common.h"
int uya_movie_play_track(u32 lsn, u32 bytes, int track);
s32 func_0023B670(s32 sector, s32 size, s32 buf, s32 state, s32 lang)
{
    (void)buf; (void)state;
    u32 bytes = (u32)size < 0x40000u ? (u32)size * 2048u : (u32)size;   /* the table gives sectors */
    uya_movie_play_track((u32)sector, bytes, lang);
    return 0;
}
