/*
 * audio_output_sdl.c -- SDL2 output for replaced DirectSound buffers, the
 * POSIX counterpart of audio_output_xaudio2.cpp (see audio_output.h).
 *
 * XAudio2 gives every slot a source voice with its own rate, a queue of
 * submitted buffers and a count of samples played. SDL gives one device, so
 * this is the small mixer that stands in for the voices: each slot keeps up
 * to QUEUE_SIZE buffers (copied, as the contract requires), the device
 * callback resamples every active slot to the device rate (linear
 * interpolation), applies its gain and sums them, and counts the source
 * frames it has consumed. That count is the slot's play position, which is
 * what the DirectSound streams pace themselves against -- the listener's
 * clock, not the wall clock.
 *
 * The same switches as the XAudio2 path: RECOMP_AUDIO_GAIN (0..1, 0 opens no
 * device), RECOMP_MUTE (plays silently, keeping the clock), RECOMP_AUDIO_DUMP.
 */
#include "audio_output.h"
#include "platform/host_sdl.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICE_COUNT     (256u + 16u + 1u)
#define QUEUE_SIZE      12u
#define MAX_BUFFER      160000u
#define DEVICE_RATE     48000
#define DEVICE_FRAMES   512

typedef struct {
    uint8_t *data;
    uint32_t capacity, bytes;
} PcmBuffer;

typedef struct {
    int active;
    uint32_t rate, channels, bits, block_align;
    float gain;
    PcmBuffer buf[QUEUE_SIZE];
    uint32_t front, queued;
    double pos;                         /* frame position in buf[front] */
    uint64_t played_frames;
} Voice;

static SDL_AudioDeviceID s_dev;
static int s_attempted, s_summary;
static float s_master = 1.0f;
static Voice s_voices[VOICE_COUNT];
static int s_reported_nonzero[VOICE_COUNT];
static unsigned long long s_submitted, s_submitted_bytes, s_nonzero, s_dropped;

static void drop(uint32_t slot, const char *reason)
{
    ++s_dropped;
    if (s_dropped <= 4 || (s_dropped & (s_dropped - 1)) == 0)
        fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
                s_dropped, slot, reason);
}

static float sample_at(const Voice *v, const PcmBuffer *b, uint32_t frame, int ch)
{
    uint32_t c = v->channels == 2 ? (uint32_t)ch : 0;
    if (v->bits == 16) {
        const int16_t *p = (const int16_t *)b->data;
        return (float)p[frame * v->channels + c] * (1.0f / 32768.0f);
    }
    return ((float)b->data[frame * v->channels + c] - 128.0f) * (1.0f / 128.0f);
}

/* The device's thread: mix every slot into `out`. */
static void SDLCALL mix(void *user, Uint8 *stream, int len)
{
    float *out = (float *)stream;
    int frames = len / (int)(sizeof(float) * 2), i;
    uint32_t s;

    (void)user;
    memset(stream, 0, (size_t)len);
    for (s = 0; s < VOICE_COUNT; s++) {
        Voice *v = &s_voices[s];
        double step;
        float gain;

        if (!v->active || !v->queued)
            continue;
        step = (double)v->rate / (double)DEVICE_RATE;
        gain = v->gain * s_master;
        for (i = 0; i < frames && v->queued; i++) {
            PcmBuffer *b = &v->buf[v->front];
            uint32_t nframes = b->bytes / v->block_align;
            uint32_t f0 = (uint32_t)v->pos, f1;
            double t = v->pos - (double)f0;
            float l0, r0, l1, r1;
            const PcmBuffer *nb = b;

            if (f0 >= nframes) {
                /* This buffer is done: the next one carries on from here. */
                v->pos -= (double)nframes;
                v->front = (v->front + 1) % QUEUE_SIZE;
                v->queued--;
                i--;
                continue;
            }
            f1 = f0 + 1;
            if (f1 >= nframes) {
                if (v->queued > 1) {
                    nb = &v->buf[(v->front + 1) % QUEUE_SIZE];
                    f1 = 0;
                } else {
                    f1 = f0;
                }
            }
            l0 = sample_at(v, b, f0, 0);
            r0 = sample_at(v, b, f0, 1);
            l1 = sample_at(v, nb, f1, 0);
            r1 = sample_at(v, nb, f1, 1);
            out[i * 2]     += gain * (l0 + (l1 - l0) * (float)t);
            out[i * 2 + 1] += gain * (r0 + (r1 - r0) * (float)t);
            {
                uint32_t before = (uint32_t)v->pos;
                v->pos += step;
                v->played_frames += (uint32_t)v->pos - before;
            }
        }
    }
    for (i = 0; i < frames * 2; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

void recomp_audio_output_initialize(void)
{
    SDL_AudioSpec want, have;
    double gain = 1.0;
    const char *setting, *mute;

    if (s_attempted)
        return;
    s_attempted = 1;
    setting = getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;
        gain = strtod(setting, &end);
        if (end == setting || *end != '\0' || !isfinite(gain) || gain < 0.0 || gain > 1.0) {
            fprintf(stderr, "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            return;
        }
    }
    if (gain == 0.0) {
        fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        return;
    }
    host_sdl_hints();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[audio-output] disabled operation=SDL_InitSubSystem error=%s\n",
                SDL_GetError());
        return;
    }
    memset(&want, 0, sizeof want);
    want.freq = DEVICE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = DEVICE_FRAMES;
    want.callback = mix;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_dev) {
        fprintf(stderr, "[audio-output] disabled operation=SDL_OpenAudioDevice error=%s\n",
                SDL_GetError());
        return;
    }
    mute = getenv("RECOMP_MUTE");
    if (mute && *mute && strcmp(mute, "0") != 0) {
        gain = 0.0;
        fprintf(stderr, "[audio-output] RECOMP_MUTE: playing silently\n");
    }
    s_master = (float)gain;
    SDL_PauseAudioDevice(s_dev, 0);
    fprintf(stderr, "[audio-output] initialized backend=sdl2 (%s, %d Hz) master_gain=%.6f\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq, gain);
}

void recomp_audio_output_shutdown(void)
{
    uint32_t s, b;

    if (s_summary)
        return;
    s_summary = 1;
    s_attempted = 1;
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    for (s = 0; s < VOICE_COUNT; s++)
        for (b = 0; b < QUEUE_SIZE; b++) {
            free(s_voices[s].buf[b].data);
            s_voices[s].buf[b].data = NULL;
        }
    fprintf(stderr,
            "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
            "nonzero_buffers=%llu dropped_buffers=%llu\n",
            s_submitted, s_submitted_bytes, s_nonzero, s_dropped);
}

void recomp_audio_output_reset_voice(uint32_t slot)
{
    Voice *v;

    if (slot >= VOICE_COUNT || !s_dev)
        return;
    v = &s_voices[slot];
    SDL_LockAudioDevice(s_dev);
    v->active = 0;
    v->queued = 0;
    v->front = 0;
    v->pos = 0.0;
    v->played_frames = 0;
    SDL_UnlockAudioDevice(s_dev);
}

int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes, uint32_t *queued_buffers)
{
    Voice *v;

    if (!s_dev || slot >= VOICE_COUNT)
        return 0;
    v = &s_voices[slot];
    if (!v->active)
        return 0;
    SDL_LockAudioDevice(s_dev);
    if (played_bytes)
        *played_bytes = v->played_frames * v->block_align;
    if (queued_buffers)
        *queued_buffers = v->queued;
    SDL_UnlockAudioDevice(s_dev);
    return 1;
}

int recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
                               uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
                               int32_t volume_hundredth_db)
{
    Voice *v;
    PcmBuffer *owned;
    uint32_t block_align, i;
    float gain;
    int nonzero = 0;
    uint8_t silence;

    recomp_audio_output_initialize();
    if (!s_dev || bytes == 0)
        return 0;
    if (slot >= VOICE_COUNT || !pcm || bytes > MAX_BUFFER || sample_rate < 1000 ||
        sample_rate > 200000 || (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16)) {
        drop(slot, "invalid-pcm");
        return 0;
    }
    block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        drop(slot, "partial-frame");
        return 0;
    }
    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    gain = volume_hundredth_db == -10000 ? 0.0f
                                         : powf(10.0f, (float)volume_hundredth_db / 2000.0f);

    v = &s_voices[slot];
    SDL_LockAudioDevice(s_dev);
    if (v->active && (v->rate != sample_rate || v->channels != channels ||
                      v->bits != bits_per_sample)) {
        /* A new format is a new voice, as with XAudio2. */
        v->active = 0;
        v->queued = 0;
        v->front = 0;
        v->pos = 0.0;
        v->played_frames = 0;
    }
    if (!v->active) {
        v->rate = sample_rate;
        v->channels = channels;
        v->bits = bits_per_sample;
        v->block_align = block_align;
        v->active = 1;
    }
    v->gain = gain;
    if (v->queued == QUEUE_SIZE) {
        SDL_UnlockAudioDevice(s_dev);
        drop(slot, "queue-full");
        return 0;
    }
    owned = &v->buf[(v->front + v->queued) % QUEUE_SIZE];
    if (owned->capacity < bytes) {
        uint8_t *data = (uint8_t *)realloc(owned->data, bytes);
        if (!data) {
            SDL_UnlockAudioDevice(s_dev);
            drop(slot, "allocation");
            return 0;
        }
        owned->data = data;
        owned->capacity = bytes;
    }
    memcpy(owned->data, pcm, bytes);
    owned->bytes = bytes;
    v->queued++;
    SDL_UnlockAudioDevice(s_dev);

    {
        static const char *dir;
        static int dir_read;
        static FILE *files[VOICE_COUNT];
        if (!dir_read) {
            dir = getenv("RECOMP_AUDIO_DUMP");
            dir_read = 1;
        }
        if (dir && *dir) {
            if (!files[slot]) {
                char path[512];
                FILE *info;
                snprintf(path, sizeof path, "%s/slot%03u.raw", dir, slot);
                files[slot] = fopen(path, "wb");
                snprintf(path, sizeof path, "%s/slot%03u.txt", dir, slot);
                if ((info = fopen(path, "w")) != NULL) {
                    fprintf(info, "rate %u channels %u bits %u first_tick_ms %u\n",
                            sample_rate, channels, bits_per_sample, (unsigned)SDL_GetTicks());
                    fclose(info);
                }
            }
            if (files[slot]) {
                fwrite(pcm, 1, bytes, files[slot]);
                fflush(files[slot]);
            }
        }
    }

    ++s_submitted;
    s_submitted_bytes += bytes;
    silence = bits_per_sample == 8 ? 0x80 : 0;
    for (i = 0; i < bytes && !nonzero; ++i)
        nonzero = pcm[i] != silence;
    if (nonzero)
        ++s_nonzero;
    if (s_submitted == 1 || (nonzero && !s_reported_nonzero[slot]))
        fprintf(stderr, "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
                        "bits=%u nonzero=%u gain=%.6f\n",
                slot, bytes, sample_rate, channels, bits_per_sample, nonzero ? 1u : 0u,
                (double)gain);
    if (nonzero)
        s_reported_nonzero[slot] = 1;
    return 1;
}
