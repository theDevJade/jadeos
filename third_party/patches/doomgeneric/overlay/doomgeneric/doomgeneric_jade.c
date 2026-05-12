/* doomgeneric_jade.c — Jade WASM platform glue for doomgeneric.
 *
 * Pairs with src/os/apps/doom_port.cpp (the C++ side that defines
 * `jade_doom_present` and routes key/mouse events from the JS canvas).
 *
 * Provides:
 *   - DG_Init / DG_DrawFrame / DG_SleepMs / DG_GetTicksMs / DG_GetKey / DG_SetWindowTitle.
 *   - jade_doom_feed_key / jade_doom_feed_mouse (called from doom_port.cpp).
 *   - DG_sound_module: software SFX mixer driving jade::audio (PCM submit).
 *     Reads DMX-format sound lumps via chocolate-doom's W_CacheLumpNum.
 *   - DG_music_module: forwarders to chocolate-doom's music_opl_module
 *     (i_oplmusic.c + midifile.c overlay) backed by third_party/opl/opl_jade.c.
 *     OPL3 samples are mixed into the SFX output buffer each Update().
 *
 * `D_GrabMouseCallback` is provided by chocolate-doom's d_main.c — do NOT
 * redefine it here.
 */

#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_sound.h"
#include "w_wad.h"
#include "opl.h"
#include "opl_jade.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  define JADE_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#  define JADE_KEEPALIVE
#endif

extern void jade_doom_present(const uint32_t* src, int sw, int sh);

/* C bridge into src/audio/jade_audio.cpp. */
extern void     jade_pcm_submit_stereo_i16(const int16_t* interleaved, uint32_t frames);
extern uint32_t jade_pcm_queued_frames(void);
extern uint32_t jade_pcm_sample_rate(void);

/* i_oplmusic.c expects this global; chocolate-doom defines it inside
 * i_sdlmusic.c which we don't compile. Driver-specific port base value is
 * unused by opl_jade. */
int opl_io_port = 0;

/* i_sound.c references these via M_BindVariable when FEATURE_SOUND is defined.
 * Upstream chocolate-doom defines them inside i_sdlsound.c (not compiled). */
int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

/* music_opl_module comes from chocolate-doom's i_oplmusic.c (overlay file). */
extern music_module_t music_opl_module;

/* -----------------------------------------------------------------------------
 * Keyboard: small lock-free queue filled by the C++ side, drained by
 * doomgeneric via DG_GetKey on each tic.
 * ---------------------------------------------------------------------------*/

#define JADE_DOOM_KEYQ_SIZE 64

static struct { unsigned char pressed; unsigned char key; } g_keyq[JADE_DOOM_KEYQ_SIZE];
static unsigned int g_keyq_head = 0u;
static unsigned int g_keyq_tail = 0u;

JADE_KEEPALIVE
void jade_doom_feed_key(int pressed, unsigned char key)
{
    if (key == 0u)
        return;
    unsigned int next = (g_keyq_tail + 1u) % JADE_DOOM_KEYQ_SIZE;
    if (next == g_keyq_head)
        return; /* full; drop newest */
    g_keyq[g_keyq_tail].pressed = (pressed != 0) ? 1u : 0u;
    g_keyq[g_keyq_tail].key     = key;
    g_keyq_tail                 = next;
}

/* -----------------------------------------------------------------------------
 * Mouse: relative motion + button state from the JS pointer-lock layer is
 * posted as a single ev_mouse event into doomgeneric's queue.
 *
 * doom_port.cpp delivers up to one call per frame with the accumulated delta;
 * buttons is a bitfield (bit 0 = left, bit 1 = right, bit 2 = middle).
 * ---------------------------------------------------------------------------*/

static unsigned int g_mouse_grab = 0u;

JADE_KEEPALIVE
void jade_doom_feed_mouse(int buttons, int dx, int dy)
{
    /* Doom expects the host to grab the cursor before posting motion (so the
     * MENU's "press F1 for the wadlist" prompt doesn't get clobbered). The
     * C++ side gates this through D_GrabMouseCallback(); we mirror that here
     * to drop motion when the game wouldn't want it. Buttons still pass
     * through so DM clicks work in menus. */
    event_t ev;
    ev.type  = ev_mouse;
    ev.data1 = buttons & 7;
    ev.data2 = g_mouse_grab ? dx : 0;
    ev.data3 = g_mouse_grab ? -dy : 0;  /* Doom Y is inverted: -dy is "forward". */
    ev.data4 = 0;
    D_PostEvent(&ev);
}

/* -----------------------------------------------------------------------------
 * doomgeneric platform hooks.
 * ---------------------------------------------------------------------------*/

void DG_Init(void) { }

void DG_DrawFrame(void)
{
    /* Snapshot the menu/grab state once per frame: this is the same predicate
     * chocolate-doom's i_video.c uses to decide whether to capture the mouse.
     * It's also the predicate doom_port.cpp's wants_pointer_lock() returns to
     * JS. */
    extern boolean menuactive;
    extern boolean paused;
    g_mouse_grab = (!menuactive && !paused) ? 1u : 0u;

    jade_doom_present(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms) { (void)ms; }

uint32_t DG_GetTicksMs(void)
{
#ifdef __EMSCRIPTEN__
    return (uint32_t)emscripten_get_now();
#else
    return 0u;
#endif
}

int DG_GetKey(int* pressed, unsigned char* key)
{
    if (g_keyq_head == g_keyq_tail)
        return 0;
    *pressed    = (int)g_keyq[g_keyq_head].pressed;
    *key        = g_keyq[g_keyq_head].key;
    g_keyq_head = (g_keyq_head + 1u) % JADE_DOOM_KEYQ_SIZE;
    return 1;
}

void DG_SetWindowTitle(const char* title) { (void)title; }

/* -----------------------------------------------------------------------------
 * SFX backend: DMX sound lumps -> 16-channel software mixer -> jade::audio.
 *
 * DMX lump layout (per chocolate-doom / Doom wiki):
 *   uint16_le  format_type   (== 3)
 *   uint16_le  sample_rate   (typically 11025 Hz)
 *   uint32_le  sample_count  (includes 32 bytes of padding)
 *   uint8[16]  leading silence (skipped)
 *   uint8[N]   8-bit unsigned PCM   (N = sample_count - 32)
 *   uint8[16]  trailing silence  (skipped)
 *
 * We linearly resample per-output-sample from src_rate to the Jade engine
 * rate (48 kHz), apply per-channel L/R volume, and accumulate into a
 * stereo int16 buffer that we submit to jade::audio's ring. After SFX is
 * packed, OPL3 music samples are additively mixed via OPL_Jade_Render_Add.
 * ---------------------------------------------------------------------------*/

#define JADE_DOOM_NUM_CHANNELS 16
/* Target ahead-of-now buffering. Keep small so cancelled sounds stop quickly. */
#define JADE_DOOM_TARGET_AHEAD_FRAMES 6000u   /* ~125 ms at 48 kHz */
/* Max output frames produced per Update() call. */
#define JADE_DOOM_MAX_MIX_FRAMES      4096u

typedef struct {
    sfxinfo_t* sfxinfo;          /* non-NULL means active */
    const uint8_t* data;         /* pointer to first PCM byte inside the lump */
    uint32_t       data_len;     /* number of 8-bit PCM bytes */
    uint32_t       src_rate;
    uint64_t       pos_q32;      /* 32.32 fixed-point: integer hi = sample index */
    uint64_t       step_q32;     /* (src_rate << 32) / out_rate */
    int32_t        vol_left;     /* 0..(127*254) */
    int32_t        vol_right;
} jade_chan_t;

static jade_chan_t g_chans[JADE_DOOM_NUM_CHANNELS];
static int         g_sfx_inited = 0;
static int         g_use_sfx_prefix = 1;
static uint32_t    g_out_rate = 48000u;

/* Per-update mix scratch (stereo int32 accumulator + int16 output buffer). */
static int32_t  g_mix_buf[JADE_DOOM_MAX_MIX_FRAMES * 2];
static int16_t  g_out_buf[JADE_DOOM_MAX_MIX_FRAMES * 2];

static uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parse DMX header. Returns 0 on success and fills out_data / out_len / out_rate. */
static int jade_parse_dmx(const uint8_t* lump, int lump_len,
                          const uint8_t** out_data, uint32_t* out_len, uint32_t* out_rate)
{
    if (!lump || lump_len < 8 + 32)
        return -1;
    uint16_t fmt   = le16(lump + 0);
    uint16_t rate  = le16(lump + 2);
    uint32_t count = le32(lump + 4);
    if (fmt != 3u)
        return -1;
    if (rate < 4000u || rate > 96000u)
        return -1;
    if (count < 32u || (int)count > lump_len - 8)
        return -1;
    *out_data = lump + 8 + 16;
    *out_len  = count - 32u;
    *out_rate = rate;
    return 0;
}

static void jade_chan_clear(jade_chan_t* c)
{
    c->sfxinfo  = NULL;
    c->data     = NULL;
    c->data_len = 0u;
    c->src_rate = 0u;
    c->pos_q32  = 0u;
    c->step_q32 = 0u;
    c->vol_left = 0;
    c->vol_right= 0;
}

static void jade_compute_pan(int vol, int sep, int32_t* l, int32_t* r)
{
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0)   sep = 0;
    if (sep > 254) sep = 254;
    int32_t left  = (int32_t)vol * (254 - sep);
    int32_t right = (int32_t)vol * sep;
    *l = left;
    *r = right;
}

/* sound_module_t hooks */

static snddevice_t jade_sfx_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

static boolean jade_snd_init(boolean use_sfx_prefix)
{
    g_use_sfx_prefix = use_sfx_prefix ? 1 : 0;
    g_out_rate       = jade_pcm_sample_rate();
    if (g_out_rate < 8000u) g_out_rate = 48000u;

    /* Force chocolate's snd_samplerate to match the Jade output rate so the
     * OPL music backend renders into the same target rate as our mixer. */
    snd_samplerate = (int)g_out_rate;

    for (int i = 0; i < JADE_DOOM_NUM_CHANNELS; ++i)
        jade_chan_clear(&g_chans[i]);
    g_sfx_inited = 1;
    return true;
}

static void jade_snd_shutdown(void)
{
    if (!g_sfx_inited)
        return;
    for (int i = 0; i < JADE_DOOM_NUM_CHANNELS; ++i)
        jade_chan_clear(&g_chans[i]);
    g_sfx_inited = 0;
}

static int jade_snd_get_sfx_lumpnum(sfxinfo_t* sfx)
{
    char name[9];
    if (g_use_sfx_prefix)
    {
        size_t n = strlen(sfx->name);
        if (n > 6) n = 6;
        name[0] = 'd'; name[1] = 's';
        memcpy(name + 2, sfx->name, n);
        name[2 + n] = '\0';
    }
    else
    {
        size_t n = strlen(sfx->name);
        if (n > 8) n = 8;
        memcpy(name, sfx->name, n);
        name[n] = '\0';
    }
    return W_GetNumForName(name);
}

static int jade_snd_start(sfxinfo_t* sfx, int channel, int vol, int sep)
{
    if (!g_sfx_inited || channel < 0 || channel >= JADE_DOOM_NUM_CHANNELS)
        return -1;
    if (sfx == NULL || sfx->lumpnum < 0)
        return -1;

    int lump_len = W_LumpLength((unsigned int)sfx->lumpnum);
    if (lump_len < 8 + 32)
        return -1;
    const uint8_t* lump = (const uint8_t*)W_CacheLumpNum(sfx->lumpnum, 1 /* PU_STATIC */);
    if (!lump)
        return -1;

    const uint8_t* data = NULL;
    uint32_t       len  = 0u;
    uint32_t       rate = 0u;
    if (jade_parse_dmx(lump, lump_len, &data, &len, &rate) != 0)
    {
        W_ReleaseLumpNum(sfx->lumpnum);
        return -1;
    }

    jade_chan_t* c = &g_chans[channel];
    /* Drop any prior sound on this channel without leaking its lump cache. */
    if (c->sfxinfo != NULL && c->sfxinfo->lumpnum >= 0)
        W_ReleaseLumpNum(c->sfxinfo->lumpnum);

    c->sfxinfo  = sfx;
    c->data     = data;
    c->data_len = len;
    c->src_rate = rate;
    c->pos_q32  = 0u;
    c->step_q32 = ((uint64_t)rate << 32) / (uint64_t)g_out_rate;
    jade_compute_pan(vol, sep, &c->vol_left, &c->vol_right);
    return channel;
}

static void jade_snd_stop(int channel)
{
    if (channel < 0 || channel >= JADE_DOOM_NUM_CHANNELS)
        return;
    jade_chan_t* c = &g_chans[channel];
    if (c->sfxinfo != NULL && c->sfxinfo->lumpnum >= 0)
        W_ReleaseLumpNum(c->sfxinfo->lumpnum);
    jade_chan_clear(c);
}

static boolean jade_snd_playing(int channel)
{
    if (channel < 0 || channel >= JADE_DOOM_NUM_CHANNELS)
        return false;
    return g_chans[channel].sfxinfo != NULL ? true : false;
}

static void jade_snd_update_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= JADE_DOOM_NUM_CHANNELS)
        return;
    jade_chan_t* c = &g_chans[channel];
    if (c->sfxinfo == NULL)
        return;
    jade_compute_pan(vol, sep, &c->vol_left, &c->vol_right);
}

static void jade_snd_cache(sfxinfo_t* sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds; /* on-demand cache via W_CacheLumpNum */
}

/* Produce `frames` stereo int16 output frames and submit to jade::audio. */
static void jade_snd_mix_and_submit(uint32_t frames)
{
    if (frames == 0u)
        return;
    if (frames > JADE_DOOM_MAX_MIX_FRAMES)
        frames = JADE_DOOM_MAX_MIX_FRAMES;

    /* Clear accumulator. */
    memset(g_mix_buf, 0, sizeof(int32_t) * (size_t)frames * 2u);

    for (int ci = 0; ci < JADE_DOOM_NUM_CHANNELS; ++ci)
    {
        jade_chan_t* c = &g_chans[ci];
        if (c->sfxinfo == NULL || c->data_len == 0u)
            continue;

        const int32_t lvol = c->vol_left;
        const int32_t rvol = c->vol_right;
        uint64_t      pos  = c->pos_q32;
        const uint64_t step = c->step_q32;
        const uint32_t end_q32_hi = c->data_len;

        for (uint32_t f = 0; f < frames; ++f)
        {
            uint32_t idx = (uint32_t)(pos >> 32);
            if (idx >= end_q32_hi)
            {
                if (c->sfxinfo->lumpnum >= 0)
                    W_ReleaseLumpNum(c->sfxinfo->lumpnum);
                jade_chan_clear(c);
                break;
            }

            /* 8-bit unsigned -> signed 16-bit (-32640..+32640). */
            int32_t s_u = (int32_t)c->data[idx];     /* 0..255 */
            int32_t s   = (s_u - 128) * 256;         /* -32768..+32512 */

            /* Headroom: >> 12 lets ~32 simultaneous full-volume sounds mix
             * without clipping, which is more than the 16-channel cap. */
            int32_t pre_l = (s * lvol) >> 12;
            int32_t pre_r = (s * rvol) >> 12;
            g_mix_buf[f * 2u + 0u] += pre_l;
            g_mix_buf[f * 2u + 1u] += pre_r;

            pos += step;
        }
        c->pos_q32 = pos;
    }

    /* Clamp SFX to int16 and pack. */
    for (uint32_t i = 0; i < frames * 2u; ++i)
    {
        int32_t s = g_mix_buf[i];
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        g_out_buf[i] = (int16_t)s;
    }

    /* Additively mix OPL3 music on top. OPL_Jade_Render_Add clamps internally,
     * advances OPL callback time, and is safe even if no music is playing
     * (returns silently when the callback queue is empty). */
    OPL_Jade_Render_Add(g_out_buf, frames);

    jade_pcm_submit_stereo_i16(g_out_buf, frames);
}

static void jade_snd_update(void)
{
    if (!g_sfx_inited)
        return;

    /* Pace ourselves against the JS-side audio context: keep ~125 ms of audio
     * buffered ahead, no more. */
    uint32_t q = jade_pcm_queued_frames();
    if (q >= JADE_DOOM_TARGET_AHEAD_FRAMES)
        return;

    uint32_t need = JADE_DOOM_TARGET_AHEAD_FRAMES - q;
    if (need > JADE_DOOM_MAX_MIX_FRAMES)
        need = JADE_DOOM_MAX_MIX_FRAMES;

    jade_snd_mix_and_submit(need);
}

sound_module_t DG_sound_module = {
    jade_sfx_devices,
    (int)(sizeof(jade_sfx_devices) / sizeof(jade_sfx_devices[0])),
    jade_snd_init,
    jade_snd_shutdown,
    jade_snd_get_sfx_lumpnum,
    jade_snd_update,
    jade_snd_update_params,
    jade_snd_start,
    jade_snd_stop,
    jade_snd_playing,
    jade_snd_cache,
};

/* -----------------------------------------------------------------------------
 * Music backend: forward to chocolate-doom's music_opl_module, defined in
 * i_oplmusic.c (overlay file). i_oplmusic drives the OPL3 emulator via
 * third_party/opl/opl_jade.c; samples are pulled into the SFX buffer above by
 * OPL_Jade_Render_Add() during each Update() poll, so no separate music
 * thread / callback is needed.
 *
 * Music modules don't get device-list filtering from i_sound.c (it always
 * assigns DG_music_module unconditionally), so the device list is essentially
 * cosmetic.
 * ---------------------------------------------------------------------------*/

static snddevice_t jade_music_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GENMIDI,
    SNDDEVICE_GUS, SNDDEVICE_AWE32,
};

static boolean jade_mus_init(void)
{
    /* Sample rate must be set before music_opl_module.Init() because that
     * function calls OPL_SetSampleRate(snd_samplerate) internally. */
    snd_samplerate = (int)jade_pcm_sample_rate();
    if (snd_samplerate < 8000) snd_samplerate = 48000;
    return music_opl_module.Init();
}
static void  jade_mus_shutdown(void)                  { music_opl_module.Shutdown(); }
static void  jade_mus_set_volume(int v)               { music_opl_module.SetMusicVolume(v); }
static void  jade_mus_pause(void)                     { music_opl_module.PauseMusic(); }
static void  jade_mus_resume(void)                    { music_opl_module.ResumeMusic(); }
static void* jade_mus_register(void* data, int len)   { return music_opl_module.RegisterSong(data, len); }
static void  jade_mus_unregister(void* h)             { music_opl_module.UnRegisterSong(h); }
static void  jade_mus_play(void* h, boolean loop)     { music_opl_module.PlaySong(h, loop); }
static void  jade_mus_stop(void)                      { music_opl_module.StopSong(); }
static boolean jade_mus_playing(void)                 { return music_opl_module.MusicIsPlaying(); }
static void  jade_mus_poll(void)
{
    if (music_opl_module.Poll != NULL)
        music_opl_module.Poll();
}

music_module_t DG_music_module = {
    jade_music_devices,
    (int)(sizeof(jade_music_devices) / sizeof(jade_music_devices[0])),
    jade_mus_init,
    jade_mus_shutdown,
    jade_mus_set_volume,
    jade_mus_pause,
    jade_mus_resume,
    jade_mus_register,
    jade_mus_unregister,
    jade_mus_play,
    jade_mus_stop,
    jade_mus_playing,
    jade_mus_poll,
};
