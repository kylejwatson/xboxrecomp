/*
 * hle_dsound.c -- DirectSound buffers replaced by name.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/dsound_service_adapter.c), GPL-3.0.
 *
 * The game's own DirectSound drives an emulated MCPX audio chip, and every
 * piece of chip behaviour it relies on has to be found before it works: the
 * codec-ready bit, the DSP doorbells, starting the chip, and then its
 * interrupt, whose handler still declined it. Burnout 2 stops a sound before
 * its attract movie and waits for IDirectSoundBuffer_GetStatus to stop saying
 * PLAYING -- 4.5 million polls, forever.
 *
 * doaxbv-re's answer, reused here: replace the buffer functions and keep each
 * buffer's play state (playing, cursor, loop, frequency) in a model on the
 * host clock (dsound_buffer_model.c), sending the samples to XAudio2
 * (audio_output_xaudio2.cpp). GetStatus then never waits on hardware.
 *
 * Differences from doaxbv-re:
 *  - keyed by XDK function name through tools.xdk_symbols, not by one title's
 *    addresses, so any title on a covered XDK gets it;
 *  - the public IDirectSoundBuffer_* entry points are replaced; creation,
 *    SetBufferData, SetVolume and the rest still run the game's own code,
 *    which keeps the settings object this reads current;
 *  - a buffer whose format cannot be played still gets a clock, so status and
 *    position always advance: doaxbv-re fell back to the original function,
 *    which here is the path that hangs;
 *  - a lock, since nothing guarantees one guest thread.
 *
 * Where the fields live (XDK 5344, Burnout 2). The interface pointer the game
 * holds is the buffer object + 0x1C, and the object keeps two settings
 * pointers: +0x1C for the buffer (data, size, play and loop regions -- read
 * by CDirectSoundBufferSettings_SetBufferData, SetLoopRegion and the
 * play-region helper) and +0x10 for the voice (format, rate, alignment,
 * volume -- written by the format packer and CDirectSoundVoice_SetVolume).
 * doaxbv-re reads all of it through the first, which is only right if both
 * point at one object; each field is read here from the one the game's own
 * code writes it to. The offsets inside match doaxbv-re's.
 */
/* The NT type vocabulary: <windows.h> on Windows, the POSIX primitives
 * (critical sections, GetTickCount64) elsewhere. */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "dsound_buffer_model.h"
#include "xbox_adpcm.h"
#include "audio_output.h"

void hle_dsound_stream_tick(uint64_t now);   /* hle_dsound_stream.c */

/* Reads a RECOMP_* switch; see xbox_memory_layout.h. Declared here rather
 * than including the kernel header, which this file has no other need of. */
int xbox_EnvSwitch(const char *name, int default_on);

enum {
    SET_FORMAT     = 0x0Cu,   /* tag | channels << 16 | bits << 24 */
    SET_RATE       = 0x10u,
    SET_ALIGN      = 0x14u,
    SET_VOLUME     = 0x1Cu,   /* hundredths of a dB, headroom applied */
    SET_DATA       = 0xB8u,
    SET_SIZE       = 0xBCu,
    SET_PLAY_START = 0xC0u,
    SET_PLAY_LEN   = 0xC4u,
    SET_LOOP_START = 0xC8u,
    SET_LOOP_LEN   = 0xCCu,

    TAG_PCM   = 0x0001u,
    TAG_ADPCM = 0x0069u,
    SLOTS     = 256u,
};

typedef struct Buffer {
    uint32_t iface;              /* the IDirectSoundBuffer the game holds */
    uint32_t data, size, format;
    int      output;             /* samples we can send to the host */
    /* IDirectSoundBuffer_Pause: the model is stopped with its cursor kept,
     * and paused_flags holds the play flags to resume with. */
    int      paused;
    uint32_t paused_flags;
    RecompDsoundBufferModel model;        /* what the game is told */
    RecompDsoundBufferModel output_model; /* what has been sent */
} Buffer;

static Buffer g_buffers[SLOTS];
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_lock(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* Buffer settings: [iface] = [object + 0x1C]. Voice settings: [object + 0x10],
 * i.e. [iface - 0x0C]. */
static int voice_field(uint32_t offset)
{
    return offset == SET_FORMAT || offset == SET_RATE ||
           offset == SET_ALIGN || offset == SET_VOLUME;
}

/* A guest address worth dereferencing: title RAM, or the contiguous window
 * where MmAllocateContiguousMemory memory lives (xbox_memory_layout.c). */
static int guest_readable(uint32_t va, uint32_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= 0x80000000u && (uint64_t)va + bytes <= 0x80000000ull + 0x04000000ull;
}

/* 0 when either pointer on the way is not guest memory. Buffer Release is not
 * replaced, so a buffer the title freed keeps its slot here until a change is
 * noticed, and its settings pointers then hold whatever reused that memory.
 * Burnout 2 crashed reading one (guest 0xFFFFD9AC) from StopEx while loading a
 * race. A 0 reads as "the buffer changed", and pump() and model_for() retire
 * the slot. */
/* The DSOUND library build, from the XBE header's library version table
 * (mapped at 0x10000: count at +0x160, table address at +0x164, 16-byte
 * entries of name[8], major, minor, build, flags). 0 if it cannot be read. */
static uint32_t dsound_build(void)
{
    static int done;
    static uint32_t build;
    uint32_t n, table, i;

    if (done)
        return build;
    done = 1;
    n = HLE_MEM32(0x10160u);
    table = HLE_MEM32(0x10164u);
    if (n > 64u || !guest_readable(table, n * 16u))
        return 0u;
    for (i = 0; i < n; i++) {
        const char *name = (const char *)HLE_PTR(table + i * 16u);
        if (memcmp(name, "DSOUND\0", 7) == 0) {
            build = HLE_MEM32(table + i * 16u + 12u) & 0xFFFFu;
            break;
        }
    }
    return build;
}

/* The buffer half of the settings moved 4 bytes later by XDK 5849
 * (TimeSplitters: Future Perfect): CDirectSoundBufferSettings_SetBufferData
 * writes the data pointer to +0xBC and the size to +0xC0, and
 * CDirectSoundBuffer_SetLoopRegion writes +0xCC/+0xD0 after checking against
 * the play length at +0xC8. Read with the 5344 offsets, the "size" was the
 * data pointer (0x8088C080...), so no ADPCM buffer got a model and every
 * sound effect was silent. The voice half (format, rate, volume) is
 * unchanged. Which build between 5344 and 5849 moved it is not known; the
 * ones in between keep the old layout until a title says otherwise. */
static uint32_t buffer_field_shift(void)
{
    static int said;
    uint32_t build = dsound_build();
    uint32_t shift = build >= 5849u ? 4u : 0u;
    if (!said++) {
        fprintf(stderr, "[DSOUND] library build %u: buffer settings at +0x%02X\n",
                build, SET_DATA + shift);
        fflush(stderr);
    }
    return shift;
}

static uint32_t setting(uint32_t iface, uint32_t offset)
{
    uint32_t holder = voice_field(offset) ? iface - 0x0Cu : iface;
    uint32_t object;

    if (!voice_field(offset))
        offset += buffer_field_shift();
    if (!guest_readable(holder, 4u))
        return 0u;
    object = HLE_MEM32(holder);
    if (!guest_readable(object + offset, 4u))
        return 0u;
    return HLE_MEM32(object + offset);
}

static uint32_t slot_of(const Buffer *b) { return (uint32_t)(b - g_buffers); }

static void retire(Buffer *b)
{
    recomp_audio_output_reset_voice(slot_of(b));
    memset(b, 0, sizeof *b);
}

/* Send the samples the output clock has consumed since the last pump. */
static void pump(Buffer *b, uint64_t now)
{
    RecompDsoundBufferModel *out = &b->output_model;
    uint32_t channels, bits, offset = 0u, bytes;
    int adpcm;
    static uint8_t pcm[80000];

    if (!b->output || !out->playing || now <= out->last_ms ||
        now - out->last_ms < 10u)
        return;
    if (b->data != setting(b->iface, SET_DATA) ||
        b->size != setting(b->iface, SET_SIZE) ||
        b->format != setting(b->iface, SET_FORMAT)) {
        retire(b);
        return;
    }
    channels = (b->format >> 16) & 0xFFu;
    bits = b->format >> 24;
    adpcm = (b->format & 0xFFFFu) == TAG_ADPCM;
    bytes = recomp_dsound_buffer_consume(out, now, &offset);
    if (bytes == 0u || b->data == 0u || bytes > sizeof pcm)
        return;

    if (adpcm) {
        uint32_t block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
        uint32_t decoded_bytes = XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        uint32_t written;
        for (written = 0u; written < bytes;) {
            int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2];
            uint32_t within = offset % decoded_bytes;
            uint32_t count = decoded_bytes - within;
            if (count > bytes - written) count = bytes - written;
            if (!xbox_adpcm_decode_block(
                    (const uint8_t *)HLE_PTR(b->data + offset / decoded_bytes * block_bytes),
                    block_bytes, channels, decoded, XBOX_ADPCM_BLOCK_SAMPLES * 2u)) {
                b->output = 0;          /* bad data: keep the clock, stop the sound */
                recomp_audio_output_reset_voice(slot_of(b));
                return;
            }
            memcpy(pcm + written, (uint8_t *)decoded + within, count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
        bits = 16u;
    } else {
        uint32_t written;
        for (written = 0u; written < bytes;) {
            uint32_t count = out->size_bytes - offset;
            if (count > bytes - written) count = bytes - written;
            memcpy(pcm + written, HLE_PTR(b->data + offset), count);
            written += count;
            offset += count;
            if (offset == out->size_bytes) offset = out->loop_start_bytes;
        }
    }
    recomp_audio_output_submit(slot_of(b), pcm, bytes, out->sample_rate,
                               channels, bits,
                               (int32_t)setting(b->iface, SET_VOLUME));
}

static Buffer *find(uint32_t iface)
{
    uint32_t i;
    if (iface == 0u) return NULL;
    for (i = 0; i < SLOTS; i++)
        if (g_buffers[i].iface == iface)
            return &g_buffers[i];
    return NULL;
}

/* The buffer's model, created or refreshed from the settings object. NULL when
 * nothing about it can be clocked (no rate or alignment worth trusting).
 *
 * `known` is the buffer if the caller already has it, else NULL to look it up.
 * DirectSoundDoWork walks all 256 slots and used to pass only the iface, so
 * every active buffer cost another full scan to find the slot the loop was
 * already standing on -- 256 slots squared, every call, on a function the
 * title calls far more often than once a frame. It was 22.7% of Dino Crisis
 * 3's on-CPU time as a leaf, which is the shape of a scan rather than of the
 * work it was meant to be doing. */
static Buffer *model_for_known(Buffer *known, uint32_t iface, uint64_t now)
{
    Buffer *b = known ? known : find(iface);
    uint32_t size, data, format, tag, channels, bits, align, rate, loop_start;
    uint32_t decoded_size, decoded_loop, block_align;
    int output;
    uint32_t i;

    if (iface == 0u || !guest_readable(iface - 0x0Cu, 0x10u) ||
        HLE_MEM32(iface) == 0u || HLE_MEM32(iface - 0x0Cu) == 0u)
        return NULL;
    size       = setting(iface, SET_SIZE);
    data       = setting(iface, SET_DATA);
    format     = setting(iface, SET_FORMAT);
    align      = setting(iface, SET_ALIGN);
    rate       = setting(iface, SET_RATE);
    loop_start = setting(iface, SET_LOOP_START);
    tag        = format & 0xFFFFu;
    channels   = (format >> 16) & 0xFFu;
    bits       = format >> 24;

    if (b && (b->data != data || b->size != size || b->format != format)) {
        retire(b);
        b = NULL;
    }
    if (align == 0u || rate < 1000u || rate > 200000u)
        return NULL;
    /* Clock whole blocks only. Burnout 2's attract-movie stream is a stereo
     * ADPCM ring of 7,876,608 bytes -- not a multiple of its 72-byte block --
     * and requiring an exact multiple left the one buffer the game waits on
     * with no model and no sound. The tail is less than one block. b->size
     * below keeps the raw size, so a real change is still noticed. */
    {
        uint32_t raw_size = size;
        size -= size % align;
        if (size == 0u)
            return NULL;
        if (b) {
            b->size = raw_size;
        }
        if (loop_start >= size)
            loop_start = 0u;
        (void)raw_size;
    }
    if (loop_start >= size || loop_start % align != 0u)
        loop_start = 0u;

    output = channels >= 1u && channels <= 2u &&
        ((tag == TAG_PCM && (bits == 8u || bits == 16u) &&
          align == channels * bits / 8u) ||
         (tag == TAG_ADPCM && bits == 4u &&
          align == XBOX_ADPCM_BLOCK_BYTES * channels));

    /* ADPCM is clocked in decoded PCM bytes, as the output consumes it. */
    if (tag == TAG_ADPCM && output) {
        uint64_t d = (uint64_t)(size / align) * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        if (d > UINT32_MAX) return NULL;
        decoded_size = (uint32_t)d;
        decoded_loop = loop_start / align * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        block_align = channels * 2u;
    } else {
        decoded_size = size;
        decoded_loop = loop_start;
        block_align = align;
    }

    if (b) {
        if (b->model.loop_start_bytes != decoded_loop) {
            pump(b, now);
            recomp_dsound_buffer_cursor(&b->model, now);
            recomp_audio_output_reset_voice(slot_of(b));
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
        }
        return b;
    }
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface == 0u) {
            b = &g_buffers[i];
            if (recomp_dsound_buffer_configure(&b->model, decoded_size, rate,
                                               block_align, now) != 0u)
                return NULL;
            b->iface = iface;
            b->data = data;
            b->size = size;
            b->format = format;
            b->output = output;
            b->model.loop_start_bytes = decoded_loop;
            b->output_model = b->model;
            return b;
        }
    }
    {
        static int said;
        if (!said++) {
            fprintf(stderr, "[DSOUND] more than %u buffers in play; "
                            "the rest are not modelled\n", SLOTS);
            fflush(stderr);
        }
    }
    return NULL;
}

static Buffer *model_for(uint32_t iface, uint64_t now)
{
    return model_for_known(NULL, iface, now);
}

static uint64_t now_ms(void) { return GetTickCount64(); }

/* After a control change, restart the output from the model unless this is a
 * Play that simply continues a sound already playing. */
static void resync_output(Buffer *b, int continuing)
{
    if (!continuing)
        recomp_audio_output_reset_voice(slot_of(b));
    if (continuing)
        b->output_model.play_flags = b->model.play_flags;
    else
        b->output_model = b->model;
}

/* HRESULT IDirectSoundBuffer_Play(this, reserved1, reserved2, flags) */
/* How often the title calls each of these.
 *
 * DirectSoundDoWork alone turned out to be 68.8 million calls in 85 seconds
 * of Dino Crisis 3 -- some 62,000 per presented frame against a console
 * budget of one -- and a count on its own does not say what the title is
 * waiting for. The ratio does: a poll loop shows up as one other entry point
 * keeping pace with DoWork, and everything else near zero.
 *
 * Reported on powers of ten of the DoWork count, so this costs a handful of
 * lines across a whole run. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

enum {
    DS_CALL_BUF_PLAY,
    DS_CALL_BUF_STOP,
    DS_CALL_BUF_STOPEX,
    DS_CALL_BUF_PAUSE,
    DS_CALL_BUF_GETSTATUS,
    DS_CALL_BUF_GETCURRENTPOSITION,
    DS_CALL_BUF_SETCURRENTPOSITION,
    DS_CALL_BUF_SETFREQUENCY,
    DS_CALL_DIRECTSOUNDDOWORK,
    DS_CALL_COUNT
};
static uint64_t g_ds_calls[DS_CALL_COUNT];
static const char *const g_ds_call_names[DS_CALL_COUNT] = {
    "IDirectSoundBuffer_Play",
    "IDirectSoundBuffer_Stop",
    "IDirectSoundBuffer_StopEx",
    "IDirectSoundBuffer_Pause/PauseEx",
    "IDirectSoundBuffer_GetStatus",
    "IDirectSoundBuffer_GetCurrentPosition",
    "IDirectSoundBuffer_SetCurrentPosition",
    "IDirectSoundBuffer_SetFrequency",
    "DirectSoundDoWork",
};

/* `caller` is the guest return address, which is the thing that actually
 * identifies the loop. DirectSoundDoWork at 810,000 calls a second is not
 * paired with any other entry point here -- GetCurrentPosition, the next
 * busiest, runs at an eighth of it -- so the counts alone say only that
 * something outside this file is calling it, and not what. */
static void ds_count_report(uint32_t caller)
{
    int i;
    fprintf(stderr, "[DSOUND] calls so far (DoWork from 0x%08X):", caller);
    for (i = 0; i < DS_CALL_COUNT; i++)
        if (g_ds_calls[i])
            fprintf(stderr, " %s=%llu", g_ds_call_names[i],
                    (unsigned long long)g_ds_calls[i]);
    fprintf(stderr, "\n");
    /* And the chain above it. The immediate caller is a game tick that
     * happens to include this call; what matters is the loop spinning on
     * that tick 58,000 times per presented frame. Same heuristic the ICALL
     * logger uses: code-range values above esp, first one most reliable. */
    {
        const uint8_t *stk = (const uint8_t *)g_xbox_mem_offset + g_esp;
        int k, shown = 0;
        fprintf(stderr, "[DSOUND]   guest stack:");
        for (k = 0; g_esp && k < 200 && shown < 8; k++) {
            uint32_t v;
            memcpy(&v, stk + (size_t)k * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none)");
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

HLE_EXPORT(IDirectSoundBuffer_Play)
{
    g_ds_calls[DS_CALL_BUF_PLAY]++;
    uint32_t iface = HLE_ARG(0), flags = HLE_ARG(3), result;
    uint64_t now = now_ms();
    static unsigned said;
    Buffer *b;
    int continuing;

    lock();
    b = model_for(iface, now);
    if (said < 16) {
        said++;
        fprintf(stderr, "[DSOUND] Play buffer=%08X format=%08X bytes=%u rate=%u "
                        "flags=%u %s\n", iface,
                iface && HLE_MEM32(iface) ? setting(iface, SET_FORMAT) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_SIZE) : 0u,
                iface && HLE_MEM32(iface) ? setting(iface, SET_RATE) : 0u, flags,
                !b ? "not modelled" : b->output ? "output" : "clock only");
        /* RECOMP_DSOUND_DUMP=1 -- the settings object, as words.
         *
         * The offsets above are one XDK's layout. When a title reports a rate
         * of 29433088 and a format of 0x01C10D60 -- both of which are guest
         * pointers, not a rate and a packed format -- the table is being read
         * against a different layout, and the only way to say which is to look
         * at what is actually there. Tony Hawk's Pro Skater 2X (XDK 3947) is
         * the first title here to disagree with it. */
        if (!b && xbox_EnvSwitch("RECOMP_DSOUND_DUMP", 0)) {
            uint32_t object = (iface && guest_readable(iface, 4u))
                            ? HLE_MEM32(iface) : 0u;
            uint32_t voice  = (iface >= 0x0Cu && guest_readable(iface - 0x0Cu, 4u))
                            ? HLE_MEM32(iface - 0x0Cu) : 0u;
            unsigned w;
            fprintf(stderr, "[DSOUND]   object=0x%08X voice=0x%08X\n",
                    object, voice);
            for (w = 0; w < 0x34u; w += 4u) {
                if (object && guest_readable(object + w, 4u))
                    fprintf(stderr, "[DSOUND]     object+%02X = %08X\n",
                            w, HLE_MEM32(object + w));
            }
            for (w = 0; w < 0x20u; w += 4u) {
                if (voice && guest_readable(voice + w, 4u))
                    fprintf(stderr, "[DSOUND]     voice +%02X = %08X\n",
                            w, HLE_MEM32(voice + w));
            }
            /* Follow whatever sits where the format and rate are expected.
             * If this build keeps a WAVEFORMATEX pointer there, its first
             * words read as tag | channels << 16, then samples per second. */
            for (w = 0x0Cu; w <= 0x10u; w += 4u) {
                uint32_t p = (voice && guest_readable(voice + w, 4u))
                           ? HLE_MEM32(voice + w) : 0u;
                unsigned k;
                if (!p || !guest_readable(p, 0x18u))
                    continue;
                fprintf(stderr, "[DSOUND]     *(voice+%02X)=0x%08X:", w, p);
                for (k = 0; k < 0x18u; k += 4u)
                    fprintf(stderr, " %08X", HLE_MEM32(p + k));
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }
    if (!b) {
        unlock();
        HLE_RETURN(iface ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
    }
    pump(b, now);
    recomp_dsound_buffer_cursor(&b->model, now);
    continuing = b->model.playing && b->output_model.playing &&
                 !(flags & RECOMP_DSOUND_PLAY_FROMSTART);
    result = recomp_dsound_buffer_play(&b->model, flags, now);
    if (result == RECOMP_DSOUND_OK) {
        b->paused = 0;
        resync_output(b, continuing);
    }
    unlock();
    HLE_RETURN(result);
}

static void stop(uint32_t iface)
{
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_stop(&b->model, now);
        b->paused = 0;
        resync_output(b, 0);
    }
    unlock();
}

/* HRESULT IDirectSoundBuffer_Stop(this) */
HLE_EXPORT(IDirectSoundBuffer_Stop)
{
    g_ds_calls[DS_CALL_BUF_STOP]++;
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* HRESULT IDirectSoundBuffer_StopEx(this, REFERENCE_TIME rtTimeStamp, flags).
 * The time stamp and envelope flags ask for a scheduled or released stop; an
 * immediate one is what the game can observe, and all it waits for. */
HLE_EXPORT(IDirectSoundBuffer_StopEx)
{
    g_ds_calls[DS_CALL_BUF_STOPEX]++;
    stop(HLE_ARG(0));
    HLE_RETURN(HLE_ARG(0) ? RECOMP_DSOUND_OK : RECOMP_DSOUND_POINTER_ERROR);
}

/* DSBPAUSE_RESUME, DSBPAUSE_PAUSE, DSBPAUSE_SYNCHPLAYBACK. */
enum { DSBPAUSE_RESUME = 0u, DSBPAUSE_PAUSE = 1u, DSBPAUSE_SYNCHPLAYBACK = 2u };

/* Pause keeps the cursor and stops the clock; resume continues from it.
 *
 * Dino Crisis 3 (XDK 5558) fades its music out by setting the volume to
 * -64 dB, calling Pause(DSBPAUSE_PAUSE), then spinning on DirectSoundDoWork
 * until GetStatus stops saying PLAYING, and only then calling Stop. Pause was
 * not replaced, so the model never heard of it, GetStatus said PLAYING for
 * ever, and the title hung for good straight after its difficulty select.
 *
 * SYNCHPLAYBACK pauses too: it holds the buffer so that several can be
 * resumed together, which from the title's side looks the same. */
static uint32_t pause_buffer(uint32_t iface, uint32_t how)
{
    uint64_t now = now_ms();
    Buffer *b;

    if (iface == 0u)
        return RECOMP_DSOUND_POINTER_ERROR;
    if (how > DSBPAUSE_SYNCHPLAYBACK)
        return RECOMP_DSOUND_INVALID_PARAM;
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (how == DSBPAUSE_RESUME) {
            if (b->paused) {
                b->paused = 0;
                if (recomp_dsound_buffer_play(&b->model, b->paused_flags, now)
                        == RECOMP_DSOUND_OK)
                    resync_output(b, 0);
            }
        } else if (b->model.playing) {
            b->paused = 1;
            b->paused_flags = b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING;
            recomp_dsound_buffer_stop(&b->model, now);
            resync_output(b, 0);
        }
    }
    unlock();
    return RECOMP_DSOUND_OK;
}

/* HRESULT IDirectSoundBuffer_Pause(this, DWORD dwPause) */
HLE_EXPORT(IDirectSoundBuffer_Pause)
{
    g_ds_calls[DS_CALL_BUF_PAUSE]++;
    HLE_RETURN(pause_buffer(HLE_ARG(0), HLE_ARG(1)));
}

/* HRESULT IDirectSoundBuffer_PauseEx(this, REFERENCE_TIME rtTimestamp,
 * DWORD dwPause). A scheduled pause is taken now, as StopEx's stop is. */
HLE_EXPORT(IDirectSoundBuffer_PauseEx)
{
    g_ds_calls[DS_CALL_BUF_PAUSE]++;
    HLE_RETURN(pause_buffer(HLE_ARG(0), HLE_ARG(3)));
}

/* HRESULT IDirectSoundBuffer_GetStatus(this, DWORD *status)
 * DSBSTATUS_PLAYING 1, DSBSTATUS_PAUSED 2, DSBSTATUS_LOOPING 4. */
HLE_EXPORT(IDirectSoundBuffer_GetStatus)
{
    g_ds_calls[DS_CALL_BUF_GETSTATUS]++;
    uint32_t iface = HLE_ARG(0), out = HLE_ARG(1), status = 0u;
    uint64_t now = now_ms();
    Buffer *b;

    if (out == 0u)
        HLE_RETURN(RECOMP_DSOUND_POINTER_ERROR);
    lock();
    b = find(iface);
    if (b) {
        pump(b, now);
        recomp_dsound_buffer_cursor(&b->model, now);
        if (b->model.playing)
            status = 1u | ((b->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
        else if (b->paused)
            status = 2u | ((b->paused_flags & RECOMP_DSOUND_PLAY_LOOPING) ? 4u : 0u);
    }
    unlock();
    HLE_MEM32(out) = status;          /* never played here: not playing */
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_GetCurrentPosition(this, DWORD *play, DWORD *write) */
HLE_EXPORT(IDirectSoundBuffer_GetCurrentPosition)
{
    g_ds_calls[DS_CALL_BUF_GETCURRENTPOSITION]++;
    uint32_t iface = HLE_ARG(0), cursor = 0u, i;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        cursor = recomp_dsound_buffer_cursor(&b->model, now);
        /* ADPCM is clocked in decoded bytes; the game asks in source bytes. */
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output)
            cursor = cursor / (XBOX_ADPCM_BLOCK_SAMPLES * b->model.block_align) *
                     (XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u);
    }
    unlock();
    for (i = 1u; i <= 2u; i++) {
        uint32_t out = HLE_ARG(i);
        if (out != 0u)
            HLE_MEM32(out) = cursor;
    }
    HLE_RETURN(RECOMP_DSOUND_OK);
}

/* HRESULT IDirectSoundBuffer_SetCurrentPosition(this, DWORD position) */
HLE_EXPORT(IDirectSoundBuffer_SetCurrentPosition)
{
    g_ds_calls[DS_CALL_BUF_SETCURRENTPOSITION]++;
    uint32_t iface = HLE_ARG(0), position = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        if ((b->format & 0xFFFFu) == TAG_ADPCM && b->output) {
            uint32_t block = XBOX_ADPCM_BLOCK_BYTES * b->model.block_align / 2u;
            if (position >= b->size || position % block != 0u)
                result = RECOMP_DSOUND_INVALID_PARAM;
            else
                position = position / block * XBOX_ADPCM_BLOCK_SAMPLES *
                           b->model.block_align;
        }
        if (result == RECOMP_DSOUND_OK)
            result = recomp_dsound_buffer_set_position(&b->model, position, now);
        if (result == RECOMP_DSOUND_OK)
            resync_output(b, 0);
    }
    unlock();
    HLE_RETURN(result);
}

/* HRESULT IDirectSoundBuffer_SetFrequency(this, DWORD frequency); 0 = default */
HLE_EXPORT(IDirectSoundBuffer_SetFrequency)
{
    g_ds_calls[DS_CALL_BUF_SETFREQUENCY]++;
    uint32_t iface = HLE_ARG(0), frequency = HLE_ARG(1), result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    Buffer *b;

    lock();
    b = model_for(iface, now);
    if (b) {
        pump(b, now);
        result = recomp_dsound_buffer_set_frequency(&b->model, frequency, now);
        if (result == RECOMP_DSOUND_OK)
            resync_output(b, 0);
    }
    unlock();
    HLE_RETURN(result);
}

/* void DirectSoundDoWork(void) -- the title's regular audio tick. The game's
 * own version services the emulated chip's deferred commands, which the
 * replaced buffers no longer need; here it feeds the host output. */
HLE_EXPORT(DirectSoundDoWork)
{
    g_ds_calls[DS_CALL_DIRECTSOUNDDOWORK]++;
    uint64_t now = now_ms();
    uint32_t i;
    static int said;

    if (!said) {
        said = 1;
        fprintf(stderr, "[DSOUND] DirectSoundDoWork replaced by name: buffers "
                        "run on the host clock and play through XAudio2\n");
        fflush(stderr);
    }
    /* How often the title asks. On the console this is a once-a-frame call;
     * anything wildly above the frame rate means the work below is being paid
     * for at a rate nobody chose, and that is worth seeing in the log rather
     * than inferring from a profile. */
    {
        /* Powers of ten, like the ICALL loggers, because a fixed interval
         * is a bug here: at 100,000 this printed 688 lines in 85 seconds
         * and NtWriteFile became 100% of the sampling profile -- the
         * counter measuring the problem became the problem. The
         * progression is the useful part anyway. The title calls this
         * 68.8 million times in 85 seconds, about 62,000 per presented
         * frame; on the console it is a once-a-frame call. */
        static uint64_t calls, next = 1;
        if (++calls >= next) {
            next *= 10;
            ds_count_report(HLE_MEM32(g_esp));
        }
    }
    lock();
    for (i = 0; i < SLOTS; i++) {
        if (g_buffers[i].iface != 0u) {
            Buffer *b = model_for_known(&g_buffers[i],
                                        g_buffers[i].iface, now);
            if (b)
                pump(b, now);
        }
    }
    unlock();
    hle_dsound_stream_tick(now);      /* streams: feed the host, complete packets */
    HLE_RETURN(0);
}
