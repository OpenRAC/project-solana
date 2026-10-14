/* Movie playback (RAC1 Wrapper).
 *
 * The game plays its movies (the intro, the cut-scenes) with Sony's sample
 * player (src/game/movie: disc streaming, the IPU, EE threads, the SPU),
 * entered through func_0023B670 (host/overrides). On the PC the whole call is
 * replaced: the movie's sectors are read from the disc image, the MPEG-2
 * video is decoded with pl_mpeg2.h and shown at the movie's frame rate with
 * its sound (PS2 ADPCM, the language track the game asks for). A skip key
 * ends it early, like a pad button on the PS2. Shared with the UYA Wrapper. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg2.h"
#include "pss.h"
#include "uya_runtime.h"

int uya_movie_play_track(uint32_t lsn, uint32_t bytes, int track)
{
    uint32_t sectors = (bytes + 2047) / 2048;
    uint8_t *ps = malloc((size_t)sectors * 2048), *es = 0;
    int skipped = 0, frames = 0;
    if (!ps || uya_disc_read(lsn, sectors, ps) != 0) { uya_log("movie at LSN %u: disc read failed", lsn); free(ps); return 0; }
    es = malloc((size_t)sectors * 2048);
    size_t n = pss_extract_video(ps, (size_t)sectors * 2048, es, (size_t)sectors * 2048);
    int16_t *pcm = 0;
    size_t pcm_frames = 0;
    int rate = 0;
    {
        uint8_t *au = malloc((size_t)sectors * 2048);
        size_t an = au ? pss_extract_audio(ps, (size_t)sectors * 2048, track, au, (size_t)sectors * 2048) : 0;
        if (!an && track && au) an = pss_extract_audio(ps, (size_t)sectors * 2048, 0, au, (size_t)sectors * 2048);
        if (an) pcm_frames = pss_decode_audio(au, an, &pcm, &rate);
        free(au);
    }
    free(ps);
    plm_video_t *v = plm_video_create_with_buffer(plm_buffer_create_with_memory(es, n, 0), 1);
    int w = plm_video_get_width(v), h = plm_video_get_height(v);
    double fps = plm_video_get_framerate(v);
    if (w <= 0 || fps <= 0) { uya_log("movie at LSN %u: no video", lsn); plm_video_destroy(v); free(es); return 0; }
    uya_log("movie at LSN %u: %ux%u at %.2f fps, %u KB; sound track %d: %s%.1f s at %d Hz", lsn, w, h, fps, (unsigned)(n / 1024), track,
            pcm_frames ? "" : "none ", pcm_frames && rate ? (double)pcm_frames / rate : 0.0, rate);
    uint32_t *px = malloc((size_t)w * h * 4);
    char tag[32];
    snprintf(tag, sizeof tag, "movie_%u", lsn);
    if (pcm_frames) plat_audio_play(pcm, pcm_frames, rate, tag);
    plat_movie_started();
    uint64_t t0 = plat_time_us();
    int nowait = getenv("UYA_NOWAIT") != 0, dropped = 0;
    plm_frame_t *f;
    while ((f = plm_video_decode(v))) {
        uint64_t due = t0 + (uint64_t)(frames * 1000000.0 / fps), now = plat_time_us();
        int late = !nowait && now > due + (uint64_t)(1000000.0 / fps);   /* a frame behind the sound: not shown, to catch up */
        if (due > now && !nowait) plat_sleep_us(due - now);
        if (!late) {
            plm_frame_to_bgra(f, (uint8_t *)px, w * 4);
            {   /* GL mode: the GS thread draws it into the window */
                extern int uya_gs_movie_frame(const uint32_t *px, int w, int h);
                int shown = uya_gs_movie_frame(px, w, h);
                if (!plat_present(shown ? 0 : px, w, h)) { uya_log("window closed"); exit(0); }
            }
        } else dropped++;
        frames++;
        if (plat_key_skip()) { skipped = 1; break; }
    }
    plat_audio_stop();
    free(pcm);
    uya_log("movie at LSN %u: %d frames shown%s (%d dropped to keep up with the sound)", lsn, frames - dropped, skipped ? " (skipped)" : "", dropped);
    free(px);
    plm_video_destroy(v);
    free(es);
    return skipped;
}
