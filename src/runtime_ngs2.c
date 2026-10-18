/* libSceNgs2 stand-ins for Hotline Miami 2.
 * - ParseWaveformData reads the game's in-memory WAV files (PCM16).
 * - Sampler voices (rack 0x1000) play PCM16 WAVs; SystemRender mixes each one
 *   into the buffer of the mastering voice (rack 0x3000) it is patched to,
 *   picked by that voice's output parameter (left/right = channels 0/1), so
 *   music and effects reach the audio ports the game meant for them. A voice
 *   with no usable route goes to the first 8-channel float buffer.
 *   ATRAC9 waveforms (music) are decoded on the fly.
 * - Port volume (param 0x2) scales each voice, so the in-game volume
 *   settings apply; a change ramps over one render block to avoid clicks.
 * - VoiceGetStateFlags reports playing/paused voices as in use: the game
 *   drops a sound it believes has ended and stops updating its volume.
 *   Pause/resume/stop events are honoured.
 * - Voice parameters are traced; only new parameter patterns are logged so
 *   the initial setup of 64+ voices does not use up the log. */
#include "runtime.h"
#include "third_party/LibAtrac9/C/src/libatrac9.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR_UNAVAILABLE ((int32_t)0x80550001)
#define ERR_INVALID_ARG ((int32_t)0x80020016)
#define MAX_RACKS 16
#define MAX_VOICES 512
#define WAVE_TYPE_PCM_I16 0x11u /* our own value: the game echoes it back */
#define WAVE_TYPE_AT9 0x12u

#define SAMPLER_RACK 0x1000u
#define MASTERING_RACK 0x3000u
/* Voice events as the game numbers them (read from its code: every voice start
 * sends 0, its release sends 1, pause/resume send 4/5) and state flag bits. */
#define EVENT_PLAY 0u
#define EVENT_STOP 1u
#define EVENT_STOP_IMM 2u
#define EVENT_KILL 3u
#define EVENT_PAUSE 4u
#define EVENT_RESUME 5u
#define STATE_INUSE 1u
#define STATE_PLAYING 2u
#define STATE_PAUSED 4u
#define OUT_RATE 48000.0

typedef struct At9 At9;
typedef struct {
    At9 *at9;                           /* streaming ATRAC9 state (music) */
    uint32_t rack_id, index;
    uint32_t type, channels, rate;      /* from the setup parameter */
    uint64_t data;                      /* guest address of the WAV file */
    uint32_t data_off, num_samples;     /* first waveform block */
    int playing, paused;
    double pos;                         /* position in source frames */
    int gain_set;                       /* port volume received; until then 1.0 */
    float gain, gain_cur;               /* target and last applied port volume */
    uint64_t dest;                      /* sampler: handle of the voice it is patched to */
    int output_set;                     /* mastering: output parameter received */
    uint32_t output_id;                 /* mastering: index of its SystemRender buffer */
    int routed;                         /* sampler: last buffer mixed into + 1 (for the log) */
} Voice;
typedef struct { uint32_t rack_id; uint32_t slot; } Rack;
typedef struct { uint32_t id, size; uint64_t hash; unsigned seen; } Seen;

static Voice voices[MAX_RACKS][MAX_VOICES];
static Rack racks[MAX_RACKS];
static int nracks;
static uint64_t h_system;
static Seen seen[512];
static int nseen;
static unsigned st_setup, st_blocks_ok, st_blocks_bad, st_play, st_play_nodata, st_stop, st_ev_other;

static void trace(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    fwrite(buf, 1, (size_t)n, stderr);
}
static void trace_hex(const char *tag, const void *p, size_t n) {
    char buf[700];
    const uint8_t *b = (const uint8_t *)p;
    size_t i, len;
    if (!p) { trace("NGS2 %s: (null)", tag); return; }
    if (n > 128) n = 128;
    len = (size_t)snprintf(buf, sizeof buf, "NGS2 %s:", tag);
    for (i = 0; i < n && len + 4 < sizeof buf; i++) len += (size_t)snprintf(buf + len, sizeof buf - len, " %02x", b[i]);
    trace("%s", buf);
}
/* Guest pointers live between 64 GiB and 1 TiB. */
#ifdef NGS2_TEST
static int plausible(uint64_t p) { return p != 0; }
#else
static int plausible(uint64_t p) { return p >= 0x100000000ull && p < 0x800000000000ull; }
#endif

static atomic_flag mix_lock = ATOMIC_FLAG_INIT;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&mix_lock, memory_order_acquire)) { } }
static void unlock(void) { atomic_flag_clear_explicit(&mix_lock, memory_order_release); }

static const Voice *as_voice(uint64_t h) {
    uint64_t lo = (uint64_t)(uintptr_t)voices;
    uint64_t hi = lo + sizeof voices;
    if (h < lo || h >= hi || (h - lo) % sizeof(Voice)) return NULL;
    return (const Voice *)(uintptr_t)h;
}
static const Rack *as_rack(uint64_t h) {
    uint64_t lo = (uint64_t)(uintptr_t)racks;
    uint64_t hi = lo + sizeof racks;
    if (h < lo || h >= hi || (h - lo) % sizeof(Rack)) return NULL;
    return (const Rack *)(uintptr_t)h;
}
static int should_log(uint32_t id, uint32_t size, uint64_t hash) {
    int i;
    for (i = 0; i < nseen; i++)
        if (seen[i].id == id && seen[i].size == size && seen[i].hash == hash) return ++seen[i].seen <= 4;
    if (nseen >= (int)(sizeof seen / sizeof *seen)) return 0;
    seen[nseen].id = id; seen[nseen].size = size; seen[nseen].hash = hash; seen[nseen].seen = 1;
    nseen++;
    return 1;
}

/* ---- WAV --------------------------------------------------------------- */
typedef struct {
    uint16_t channels, bits, block_align;
    uint32_t rate, data_off, data_size;
    int at9, has_loop;
    uint8_t config[4];
    uint32_t total, delay, loop_start, loop_end;
} Wav;
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

static const uint8_t AT9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                     0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
/* 0 = ok, -1 = not a usable WAV, -2 = valid WAV but not PCM16 / ATRAC9 mono/stereo. */
static int parse_wav(const uint8_t *d, size_t n, Wav *w) {
    size_t pos = 12;
    int have_fmt = 0, have_data = 0;
    uint16_t tag = 0;
    memset(w, 0, sizeof *w);
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    while (pos + 8 <= n) {
        uint32_t sz = le32(d + pos + 4);
        const uint8_t *c = d + pos + 8;
        size_t avail = n - pos - 8;
        if (!memcmp(d + pos, "fmt ", 4) && sz >= 16 && avail >= 16) {
            tag = le16(c); w->channels = le16(c + 2); w->rate = le32(c + 4); w->bits = le16(c + 14);
            w->block_align = le16(c + 12);
            have_fmt = 1;
            if (tag == 0xFFFE && sz >= 52 && avail >= 52 && !memcmp(c + 24, AT9_GUID, 16)) {
                w->at9 = 1;
                memcpy(w->config, c + 44, 4);
            }
        } else if (!memcmp(d + pos, "fact", 4) && sz >= 4 && avail >= 4) {
            w->total = le32(c);
            if (sz >= 12 && avail >= 12) w->delay = le32(c + 8);
        } else if (!memcmp(d + pos, "smpl", 4) && sz >= 60 && avail >= 60) {
            if (le32(c + 28) >= 1) {
                w->has_loop = 1;
                w->loop_start = le32(c + 44);
                w->loop_end = le32(c + 48);
            }
        } else if (!memcmp(d + pos, "data", 4)) {
            w->data_off = (uint32_t)(pos + 8);
            w->data_size = sz > avail ? (uint32_t)avail : sz;
            have_data = 1;
            break;
        }
        if (sz > avail) break;
        pos += 8 + (size_t)sz + (sz & 1u);
    }
    if (!have_fmt || !have_data) return -1;
    if (w->delay > 8192) w->delay = 0;
    if (w->at9) return (w->channels >= 1 && w->channels <= 2) ? 0 : -2;
    if ((tag != 1 && tag != 0xFFFE) || w->bits != 16 || w->channels < 1 || w->channels > 2) return -2;
    return 0;
}
static void put32(uint8_t *info, int idx, uint32_t v) { memcpy(info + (size_t)idx * 4, &v, 4); }

/* ---- ATRAC9 music: decoded on the fly, one frame at a time ------------- */
struct At9 {
    void *h;
    Atrac9CodecInfo info;
    uint8_t config[4];
    const uint8_t *data;       /* raw superframes */
    uint32_t data_size;
    uint32_t delay, total, loop_start, loop_end;
    int has_loop, failed;
    uint32_t ip, sf_start;     /* byte offsets inside data */
    int frames_in_sf;
    int16_t pcm[2048 * 2];
    uint32_t fcount, fpos;     /* decoded samples in pcm / read position */
    uint32_t discard;          /* decoded samples still to drop (delay, seek) */
    uint32_t spos;             /* logical sample position */
    double acc;
    float l, r;
};
static void at9_free(Voice *v) {
    if (!v->at9) return;
    if (v->at9->h) Atrac9ReleaseHandle(v->at9->h);
    free(v->at9);
    v->at9 = NULL;
}
static void at9_seek(At9 *a, uint32_t target) {
    uint32_t sfl = (uint32_t)a->info.framesInSuperframe * (uint32_t)a->info.frameSamples;
    uint32_t abs = target + a->delay, sf = abs / sfl, start = sf > 0 ? sf - 1 : 0;
    if (a->h) Atrac9ReleaseHandle(a->h);
    a->h = Atrac9GetHandle();
    if (!a->h || Atrac9InitDecoder(a->h, a->config) != 0) { a->failed = 1; return; }
    a->sf_start = a->ip = start * (uint32_t)a->info.superframeSize;
    a->frames_in_sf = 0;
    a->fcount = a->fpos = 0;
    a->discard = abs - start * sfl;
    a->spos = target;
    a->failed = 0;
}
static At9 *at9_open(const uint8_t *file, const Wav *w, uint32_t data_off, uint32_t data_size, uint32_t total) {
    At9 *a = (At9 *)calloc(1, sizeof *a);
    if (!a) return NULL;
    memcpy(a->config, w->config, 4);
    a->h = Atrac9GetHandle();
    if (!a->h || Atrac9InitDecoder(a->h, a->config) != 0 || Atrac9GetCodecInfo(a->h, &a->info) != 0 ||
        a->info.channels < 1 || a->info.channels > 2 || a->info.frameSamples < 1 || a->info.frameSamples > 2048 ||
        a->info.superframeSize < 1 || a->info.framesInSuperframe < 1) {
        if (a->h) Atrac9ReleaseHandle(a->h);
        free(a);
        return NULL;
    }
    a->data = file + data_off;
    a->data_size = data_size;
    a->delay = w->delay;
    a->total = total;
    a->has_loop = w->has_loop && w->loop_end > w->loop_start;
    /* loop points in the file count the encoder delay; ours do not */
    a->loop_start = w->loop_start >= w->delay ? w->loop_start - w->delay : 0;
    a->loop_end = w->loop_end >= w->delay ? w->loop_end - w->delay : 0;
    trace("NGS2 ATRAC9 open: %d ch, %d Hz, frame %d samples, %d frames/superframe, superframe %d bytes, total=%u delay=%u loop=%s [%u..%u]",
          a->info.channels, a->info.samplingRate, a->info.frameSamples, a->info.framesInSuperframe,
          a->info.superframeSize, (unsigned)total, (unsigned)a->delay, a->has_loop ? "yes" : "no",
          (unsigned)a->loop_start, (unsigned)a->loop_end);
    at9_seek(a, 0);
    return a;
}
/* Next stereo sample; 0 at the end of the track (or on a decode error). */
static int at9_next(At9 *a, float *l, float *r) {
    uint32_t end = a->has_loop ? a->loop_end + 1 : a->total;
    if (a->total && end > a->total) end = a->total;
    if (a->failed) return 0;
    if (end && a->spos >= end) {
        if (!a->has_loop) return 0;
        at9_seek(a, a->loop_start);
        if (a->failed) return 0;
    }
    while (a->fpos >= a->fcount) {
        int used = 0;
        if (a->ip >= a->data_size) return 0;
        if (Atrac9Decode(a->h, a->data + a->ip, (short *)a->pcm, &used) != 0 || used <= 0) { a->failed = 1; return 0; }
        a->ip += (uint32_t)used;
        a->fcount = (uint32_t)a->info.frameSamples;
        a->fpos = 0;
        if (++a->frames_in_sf >= a->info.framesInSuperframe) {
            a->sf_start += (uint32_t)a->info.superframeSize;
            a->ip = a->sf_start;
            a->frames_in_sf = 0;
        }
        if (a->discard) {
            uint32_t dd = a->discard < a->fcount ? a->discard : a->fcount;
            a->fpos = dd;
            a->discard -= dd;
        }
    }
    {
        size_t idx = (size_t)a->fpos * (size_t)a->info.channels;
        *l = (float)a->pcm[idx] / 32768.0f;
        *r = a->info.channels == 2 ? (float)a->pcm[idx + 1] / 32768.0f : *l;
    }
    a->fpos++;
    a->spos++;
    return 1;
}
/* ---- Volume ------------------------------------------------------------ */
static float gain_target(const Voice *v) { return v->gain_set ? v->gain : 1.0f; }
static void set_gain(Voice *v, float level) {
    static unsigned n;
    if (!(level >= 0.0f && level <= 4.0f)) return;  /* also rejects NaN */
    if (v->gain_set && v->gain == level) return;
    if (!v->gain_set) v->gain_cur = 1.0f;          /* ramp from the implicit 1.0 */
    v->gain_set = 1;
    v->gain = level;
    if (n++ < 60) trace("NGS2 VOLUME voice=%u level=%.3f", v->index, (double)level);
}
/* Gain for frame 0 and the per-frame step that reaches the target at the end of the block. */
static float gain_ramp(Voice *v, uint32_t frames, float *step) {
    const float g0 = v->gain_set ? v->gain_cur : 1.0f, g1 = gain_target(v);
    *step = frames ? (g1 - g0) / (float)frames : 0.0f;
    v->gain_cur = g1;
    return g0;
}

/* Adds one stereo frame to an interleaved buffer of `ch` channels (mono buffers get the average). */
static void add_frame(float *dst, uint32_t i, uint32_t ch, float l, float r) {
    if (ch >= 2) { dst[(size_t)i * ch] += l; dst[(size_t)i * ch + 1] += r; }
    else dst[i] += 0.5f * (l + r);
}

static void mix_at9(Voice *v, float *dst, uint32_t frames, uint32_t ch) {
    At9 *a = v->at9;
    const double step = (double)v->rate / OUT_RATE;
    float dg, g = gain_ramp(v, frames, &dg);
    uint32_t i;
    for (i = 0; i < frames; i++) {
        a->acc += step;
        while (a->acc >= 1.0) {
            if (!at9_next(a, &a->l, &a->r)) { v->playing = 0; return; }
            a->acc -= 1.0;
        }
        g += dg;
        add_frame(dst, i, ch, a->l * g, a->r * g);
    }
}

/* ---- Sampler voices ---------------------------------------------------- */
static void apply_param(Voice *v, const uint8_t *p, uint32_t size, uint32_t id) {
    static unsigned plays;
    if (v->rack_id == MASTERING_RACK) {
        if (id == 0x30000005u && size >= 12) { v->output_id = le32(p + 8); v->output_set = 1; }  /* output */
        return;
    }
    if (v->rack_id != SAMPLER_RACK) return;
    if (id == 0x10000000u && size >= 20) {            /* setup: waveform format */
        v->type = le32(p + 8); v->channels = le32(p + 12); v->rate = le32(p + 16);
        at9_free(v);
        v->data = 0; v->num_samples = 0; v->playing = 0; v->paused = 0; v->pos = 0;
        st_setup++;
    } else if (id == 0x10000001u && size >= 32) {     /* waveform blocks */
        uint64_t data = le64(p + 8), blocks = le64(p + 24);
        uint32_t nb = le32(p + 20);
        at9_free(v);
        v->data = 0; v->num_samples = 0; v->playing = 0; v->paused = 0; v->pos = 0;
        if (nb >= 1 && plausible(data) && plausible(blocks) && v->channels >= 1 && v->channels <= 2 && v->rate > 0) {
            const uint8_t *b = (const uint8_t *)(uintptr_t)blocks;
            uint32_t size_bytes = le32(b + 4), ns = le32(b + 16), off = le32(b);
            if (v->type == WAVE_TYPE_PCM_I16) {
                uint32_t maxns = size_bytes / (2u * v->channels);
                v->data_off = off;
                v->num_samples = (ns && ns <= maxns) ? ns : maxns;
                v->data = data;
            } else if (v->type == WAVE_TYPE_AT9 && off >= 12 && off < (1u << 20)) {
                Wav w;
                const uint8_t *file = (const uint8_t *)(uintptr_t)data;
                if (parse_wav(file, (size_t)off + 8, &w) == 0 && w.at9 && w.data_off == off) {
                    uint32_t total = ns ? ns : w.total;
                    v->at9 = at9_open(file, &w, off, size_bytes, total);
                    if (v->at9) { v->data_off = off; v->num_samples = total ? total : 0x7fffffffu; v->data = data; }
                }
            }
        }
        if (v->data) st_blocks_ok++; else st_blocks_bad++;
    } else if (id == 0x00000002u && size >= 16) {     /* port volume: port, linear level */
        float level;
        memcpy(&level, p + 12, 4);
        if (le32(p + 8) == 0) set_gain(v, level);
    } else if (id == 0x00000005u && size >= 24) {     /* patch: port, dest input, dest handle */
        if (le32(p + 8) == 0) v->dest = le64(p + 16);
    } else if (id == 0x00000006u && size >= 12) {     /* event */
        static unsigned others;
        uint32_t ev = le32(p + 8);
        if (ev == EVENT_PLAY) {
            st_play++;
            if (!(v->data && v->num_samples)) st_play_nodata++;
            if (v->data && v->num_samples) {
                v->playing = 1; v->paused = 0; v->pos = 0;
                v->gain_cur = gain_target(v);
                if (v->at9) { at9_seek(v->at9, 0); v->at9->acc = 0.0; if (v->at9->failed) v->playing = 0; }
                if (plays++ < 60)
                    trace("NGS2 PLAY voice=%u ch=%u rate=%u samples=%u", v->index, v->channels, v->rate, v->num_samples);
            }
        } else {
            if (ev == EVENT_STOP) st_stop++; else st_ev_other++;
            if (ev == EVENT_STOP || ev == EVENT_STOP_IMM || ev == EVENT_KILL) { v->playing = 0; v->paused = 0; }
            else if (ev == EVENT_PAUSE) v->paused = v->playing;
            else if (ev == EVENT_RESUME) v->paused = 0;
            if (ev != EVENT_STOP && others++ < 60)   /* a stop ends every sound: counted, not logged */
                trace("NGS2 EVENT %u voice=%u -> %s", ev, v->index,
                      !v->playing ? "stopped" : v->paused ? "paused" : "playing");
        }
    }
}
static float pcm(const uint8_t *base, uint32_t frame, uint32_t ch, uint32_t c) {
    int16_t s;
    memcpy(&s, base + ((size_t)frame * ch + c) * 2, 2);
    return (float)s / 32768.0f;
}
static void mix_voice(Voice *v, float *dst, uint32_t frames, uint32_t out_ch) {
    if (v->at9) { mix_at9(v, dst, frames, out_ch); return; }
    const uint8_t *base = (const uint8_t *)(uintptr_t)v->data + v->data_off;
    const uint32_t ch = v->channels;
    const double step = (double)v->rate / OUT_RATE;
    float dg, g = gain_ramp(v, frames, &dg);
    uint32_t i;
    for (i = 0; i < frames; i++) {
        uint32_t i0 = (uint32_t)v->pos, i1;
        float frac, l, r;
        if (i0 >= v->num_samples) { v->playing = 0; return; }
        i1 = i0 + 1 < v->num_samples ? i0 + 1 : i0;
        frac = (float)(v->pos - (double)i0);
        l = pcm(base, i0, ch, 0) * (1.0f - frac) + pcm(base, i1, ch, 0) * frac;
        r = ch == 2 ? pcm(base, i0, ch, 1) * (1.0f - frac) + pcm(base, i1, ch, 1) * frac : l;
        g += dg;
        add_frame(dst, i, out_ch, l * g, r * g);
        v->pos += step;
    }
}

/* ---- Routing ------------------------------------------------------------ */
typedef struct { float *p; uint32_t frames, ch; } Out;
/* SystemRender buffer index of the mastering voice a sampler voice is patched to, or -1. */
static int voice_output(const Voice *v) {
    const Voice *m = v->dest ? as_voice(v->dest) : NULL;
    return (m && m->rack_id == MASTERING_RACK && m->output_set) ? (int)m->output_id : -1;
}
static void clip(const Out *o) {
    uint32_t i, c, n = o->ch < 2 ? o->ch : 2;
    for (i = 0; i < o->frames; i++)
        for (c = 0; c < n; c++) {
            float *s = &o->p[(size_t)i * o->ch + c];
            if (*s > 1.0f) *s = 1.0f; else if (*s < -1.0f) *s = -1.0f;
        }
}

/* ---- API --------------------------------------------------------------- */
static ABI int32_t ngs2_system_create(uint64_t opt, uint64_t alloc, uint64_t *out) {
    static int n;
    if (n++ < 10) {
        trace("NGS2 SystemCreate option=%llx allocator=%llx", (unsigned long long)opt, (unsigned long long)alloc);
        trace_hex("SystemCreate.option", (const void *)(uintptr_t)opt, 32);
    }
    if (!out) return ERR_INVALID_ARG;
    *out = (uint64_t)(uintptr_t)&h_system;
    return 0;
}
static ABI int32_t ngs2_system_destroy(uint64_t sys) {
    (void)sys;
    trace("NGS2 SystemDestroy");
    return 0;
}
static ABI int32_t ngs2_rack_create(uint64_t sys, uint64_t rack_id, uint64_t opt, uint64_t alloc, uint64_t *out) {
    int i;
    (void)sys; (void)alloc;
    trace("NGS2 RackCreate rackId=0x%llx", (unsigned long long)rack_id);
    trace_hex("RackCreate.option", (const void *)(uintptr_t)opt, 64);
    if (!out) return ERR_INVALID_ARG;
    if (nracks >= MAX_RACKS) return ERR_UNAVAILABLE;
    i = nracks++;
    racks[i].rack_id = (uint32_t)rack_id;
    racks[i].slot = (uint32_t)i;
    *out = (uint64_t)(uintptr_t)&racks[i];
    return 0;
}
static ABI int32_t ngs2_rack_destroy(uint64_t rack) {
    const Rack *r = as_rack(rack);
    trace("NGS2 RackDestroy rack=0x%x", r ? r->rack_id : 0u);
    return 0;
}
static unsigned max_voice[MAX_RACKS];
static ABI int32_t ngs2_rack_get_voice(uint64_t rack, uint64_t voice_id, uint64_t *out) {
    static unsigned failures;
    const Rack *r = as_rack(rack);
    if (!out || !r || voice_id >= MAX_VOICES) {
        if (failures++ < 5) trace("NGS2 RackGetVoiceHandle FAILED rack=%llx voice=%llu", (unsigned long long)rack, (unsigned long long)voice_id);
        return ERR_INVALID_ARG;
    }
    voices[r->slot][voice_id].rack_id = r->rack_id;
    voices[r->slot][voice_id].index = (uint32_t)voice_id;
    if (voice_id >= max_voice[r->slot]) max_voice[r->slot] = (unsigned)voice_id + 1;
    *out = (uint64_t)(uintptr_t)&voices[r->slot][voice_id];
    return 0;
}
static ABI int32_t ngs2_voice_control(uint64_t voice, uint64_t list) {
    static unsigned total;
    const Voice *v = as_voice(voice);
    const uint8_t *p = (const uint8_t *)(uintptr_t)list;
    int k;
    if (!p) return 0;
    for (k = 0; k < 8; k++) {
        uint16_t size = le16(p);
        int16_t next = (int16_t)le16(p + 2);
        uint32_t id = le32(p + 4);
        uint64_t hash = 1469598103934665603ull;
        size_t i, from = id == 0x10000001u ? 16 : 8, to = id == 0x10000001u ? 24 : size;
        if (v && size >= 8 && size <= 128) {
            Voice *mv = &voices[0][0] + (v - &voices[0][0]);
            lock(); apply_param(mv, p, size, id); unlock();
        }
        if (size >= 8 && size <= 128) {
            for (i = from; i < to && i < size; i++) hash = (hash ^ p[i]) * 1099511628211ull;
            if (total < 4000 && should_log(id, size, hash)) {
                char tag[96];
                total++;
                snprintf(tag, sizeof tag, "VoiceControl rack=0x%x voice=%u param#%d id=0x%08x size=%u next=%d",
                         v ? v->rack_id : 0u, v ? v->index : 0u, k, (unsigned)id, (unsigned)size, (int)next);
                trace_hex(tag, p, size);
                if (id == 0x10000001u && size >= 32) {
                    uint64_t data, blocks;
                    uint32_t nblocks = le32(p + 20);
                    memcpy(&data, p + 8, 8);
                    memcpy(&blocks, p + 24, 8);
                    if (nblocks && nblocks <= 8) {
                        if (plausible(data)) trace_hex("   waveform.data", (const void *)(uintptr_t)data, 16);
                        if (plausible(blocks)) trace_hex("   waveform.blocks", (const void *)(uintptr_t)blocks, 64);
                    }
                }
            }
        }
        if (next <= 0 || next > 0x400 || size < 8) break;
        p += next;
    }
    return 0;
}
static ABI int32_t ngs2_voice_state_flags(uint64_t voice, uint32_t *flags) {
    static unsigned n;
    const Voice *v = as_voice(voice);
    uint32_t f = 0;
    if (!flags) return ERR_INVALID_ARG;
    lock();
    if (v && v->playing) f = v->paused ? (STATE_INUSE | STATE_PAUSED) : (STATE_INUSE | STATE_PLAYING);
    unlock();
    if (n < 3 || n % 2000 == 0)
        trace("NGS2 VoiceGetStateFlags call#%u rack=0x%x voice=%u -> 0x%x", n, v ? v->rack_id : 0u, v ? v->index : 0u, (unsigned)f);
    n++;
    *flags = f;
    return 0;
}
static ABI int32_t ngs2_parse_waveform(uint64_t data, uint64_t size, uint8_t *info) {
    static unsigned n, rejected;
    Wav w;
    int rc;
    uint32_t nsamples, unit;
    if (!data || !info || size < 12 || size > 0x40000000u) return ERR_INVALID_ARG;
    rc = parse_wav((const uint8_t *)(uintptr_t)data, (size_t)size, &w);
    if (rc) {
        trace("NGS2 ParseWaveformData size=%llu REJECTED (%s)", (unsigned long long)size, rc == -2 ? "not PCM16/ATRAC9 mono/stereo" : "not a WAV");
        trace_hex("ParseWaveformData.data", (const void *)(uintptr_t)data, rejected++ < 4 ? 128 : 48);
        return ERR_UNAVAILABLE;
    }
    unit = w.at9 ? (w.block_align ? w.block_align : 1u) : 2u * w.channels;
    nsamples = w.at9 ? (w.total ? w.total : (w.data_size / unit) * 1024u) : w.data_size / unit;
    memset(info, 0, 96);
    put32(info, 0, w.at9 ? WAVE_TYPE_AT9 : WAVE_TYPE_PCM_I16); put32(info, 1, w.channels); put32(info, 2, w.rate);
    put32(info, 6, w.data_off); put32(info, 7, w.data_size);
    put32(info, 10, nsamples); put32(info, 11, unit); put32(info, 12, 1); put32(info, 13, 1);
    put32(info, 14, unit); put32(info, 15, 1); put32(info, 17, 1);
    put32(info, 18, w.data_off); put32(info, 19, w.data_size); put32(info, 22, nsamples);
    if (w.at9) trace("NGS2 ParseWaveformData ATRAC9 ok size=%llu ch=%u rate=%u samples=%u blockAlign=%u delay=%u loop=%d",
                     (unsigned long long)size, (unsigned)w.channels, (unsigned)w.rate, (unsigned)nsamples,
                     (unsigned)w.block_align, (unsigned)w.delay, w.has_loop);
    else if (n++ < 12) trace("NGS2 ParseWaveformData ok size=%llu ch=%u rate=%u dataOff=%u dataSize=%u samples=%u",
                        (unsigned long long)size, (unsigned)w.channels, (unsigned)w.rate, (unsigned)w.data_off,
                        (unsigned)w.data_size, (unsigned)nsamples);
    return 0;
}
static ABI int32_t ngs2_render(uint64_t sys, uint64_t buffers, uint64_t count) {
    static unsigned calls, routes;
    const uint8_t *b = (const uint8_t *)(uintptr_t)buffers;
    Out outs[16];
    float *main_buf = NULL;
    uint32_t frames = 0;
    uint64_t i;
    int main_out = -1;
    unsigned active = 0;
    (void)sys;
    { static unsigned entry; if (entry < 5) { entry++;
      trace("NGS2 RENDER ENTRY #%u sys=%llx buffers=%llx count=%llu", entry,
            (unsigned long long)sys, (unsigned long long)buffers, (unsigned long long)count); } }
    if (!plausible(buffers) || count == 0 || count > 16) return 0;
    memset(outs, 0, sizeof outs);
    for (i = 0; i < count; i++) {
        uint64_t ptr, size;
        uint32_t type, ch;
        memcpy(&ptr, b + i * 24, 8);
        memcpy(&size, b + i * 24 + 8, 8);
        memcpy(&type, b + i * 24 + 16, 4);
        memcpy(&ch, b + i * 24 + 20, 4);
        if (calls == 0) trace("NGS2 SystemRender buffer[%llu] ptr=%llx size=%llu type=0x%x channels=%u", (unsigned long long)i,
                              (unsigned long long)ptr, (unsigned long long)size, (unsigned)type, (unsigned)ch);
        if (!plausible(ptr) || size == 0 || size > (1u << 20)) continue;
        memset((void *)(uintptr_t)ptr, 0, (size_t)size);
        if (type == 0x18 && ch >= 1 && ch <= 8) {
            outs[i].p = (float *)(uintptr_t)ptr;
            outs[i].ch = ch;
            outs[i].frames = (uint32_t)(size / (4u * ch));
            if (main_out < 0 && ch == 8) main_out = (int)i;
        }
    }
    if (main_out >= 0) {
        main_buf = outs[main_out].p;
        frames = outs[main_out].frames;
    }
    if (main_buf && frames) {
        int r;
        lock();
        for (r = 0; r < nracks; r++) {
            unsigned vi;
            if (racks[r].rack_id != SAMPLER_RACK) continue;
            for (vi = 0; vi < max_voice[r]; vi++) {
                Voice *v = &voices[r][vi];
                int o;
                if (!v->playing || v->paused) continue;
                o = voice_output(v);
                if (o < 0 || o >= (int)count || !outs[o].p) o = main_out;
                if (v->routed != o + 1) {
                    v->routed = o + 1;
                    if (routes++ < 60) trace("NGS2 ROUTE voice=%u -> buffer %d", v->index, o);
                }
                mix_voice(v, outs[o].p, outs[o].frames, outs[o].ch);
                active++;
            }
        }
        unlock();
        for (i = 0; i < count; i++) if (outs[i].p) clip(&outs[i]);
    }
    if (main_buf && frames) {
        static int beep_on = -1;
        static float bphase;
        if (beep_on < 0) {
            const char *e = getenv("NGS2_BEEP");
            beep_on = (e && *e == '1') ? 1 : 0;
            if (beep_on) trace("NGS2 BEEP test tone enabled (first ~10 seconds)");
        }
        if (beep_on && calls < 2000) {
            uint32_t k;
            for (k = 0; k < frames; k++) {
                float s;
                bphase += 440.0f / 48000.0f;
                if (bphase >= 1.0f) bphase -= 1.0f;
                s = bphase < 0.5f ? 0.1f : -0.1f;
                main_buf[(size_t)k * 8] += s;
                main_buf[(size_t)k * 8 + 1] += s;
            }
        }
    }
    if (calls == 0 || calls == 99 || calls == 999 || (calls + 1) % 4000 == 0) {
        int r;
        trace("NGS2 SystemRender call#%u active voices=%u", calls + 1, active);
        trace("NGS2 STATS setup=%u blocks_ok=%u blocks_bad=%u play=%u play_without_data=%u stop=%u other_events=%u",
              st_setup, st_blocks_ok, st_blocks_bad, st_play, st_play_nodata, st_stop, st_ev_other);
        for (r = 0; r < nracks; r++) trace("NGS2   rack 0x%x: %u voice handles handed out", racks[r].rack_id, max_voice[r]);
    }
    calls++;
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceNgs2SystemCreateWithAllocator", ngs2_system_create},
    {"sceNgs2SystemDestroy", ngs2_system_destroy},
    {"sceNgs2RackCreateWithAllocator", ngs2_rack_create},
    {"sceNgs2RackDestroy", ngs2_rack_destroy},
    {"sceNgs2RackGetVoiceHandle", ngs2_rack_get_voice},
    {"sceNgs2VoiceControl", ngs2_voice_control},
    {"sceNgs2VoiceGetStateFlags", ngs2_voice_state_flags},
    {"sceNgs2ParseWaveformData", ngs2_parse_waveform},
    {"sceNgs2_unk_i0VnXM", ngs2_render}, /* really sceNgs2SystemRender */
};

uintptr_t runtime_ngs2_resolve(const char *nid) { return RUNTIME_LOOKUP(exports, nid); }
