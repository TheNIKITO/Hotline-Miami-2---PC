/* libScePngDec for Hotline Miami 2: a small self-contained PNG decoder
 * (own inflate, no external libraries). Output is RGBA8 or BGRA8. */
#include "runtime.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PNG_ERR_ADDR ((int32_t)0x80690001)
#define PNG_ERR_SIZE ((int32_t)0x80690002)
#define PNG_ERR_HANDLE ((int32_t)0x80690004)
#define PNG_ERR_DATA ((int32_t)0x80690010)
#define PNG_ERR_UNSUPPORTED ((int32_t)0x80690011)
#define PNG_ERR_DECODE ((int32_t)0x80690012)

typedef struct { uint32_t thisSize, attribute, maxImageWidth; } PngCreateParam;
typedef struct { const void *pngMemAddr; uint32_t pngMemSize, reserved; } PngParseParam;
typedef struct { uint32_t imageWidth, imageHeight; uint16_t colorSpace, bitDepth; uint32_t imageFlag; } PngImageInfo;
typedef struct {
    const void *pngMemAddr; void *imageMemAddr;
    uint32_t pngMemSize, imageMemSize;
    uint16_t pixelFormat, alphaValue;
    uint32_t imagePitch;
} PngDecodeParam;

/* ---- inflate (RFC 1951) ------------------------------------------------ */
#define MAXBITS 15
typedef struct {
    const uint8_t *in; size_t inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outpos;
} Inf;
typedef struct { short count[MAXBITS + 1]; short symbol[288]; } Huff;

static int inf_bits(Inf *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) return -1;
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}
static int inf_decode(Inf *s, const Huff *h) {
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int inf_construct(Huff *h, const short *length, int n) {
    int symbol, len, left;
    short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1; left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}
static const short lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inf_codes(Inf *s, const Huff *lc, const Huff *dc) {
    int symbol, len, b;
    size_t dist;
    do {
        symbol = inf_decode(s, lc);
        if (symbol < 0) return -1;
        if (symbol < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return -1;
            b = inf_bits(s, lext[symbol]);
            if (b < 0) return -1;
            len = lens[symbol] + b;
            symbol = inf_decode(s, dc);
            if (symbol < 0 || symbol >= 30) return -1;
            b = inf_bits(s, dext[symbol]);
            if (b < 0) return -1;
            dist = (size_t)(dists[symbol] + b);
            if (dist > s->outpos) return -1;
            if (s->outpos + (size_t)len > s->outlen) return -1;
            while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
    } while (symbol != 256);
    return 0;
}
static int inf_stored(Inf *s) {
    size_t len, nlen;
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    len = (size_t)s->in[s->inpos] | ((size_t)s->in[s->inpos + 1] << 8);
    nlen = (size_t)s->in[s->inpos + 2] | ((size_t)s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if (len != (~nlen & 0xffffu)) return -1;
    if (s->inpos + len > s->inlen || s->outpos + len > s->outlen) return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len; s->outpos += len;
    return 0;
}
static int inf_fixed(Inf *s) {
    Huff lc, dc;
    short lengths[288];
    int sym;
    for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < 288; sym++) lengths[sym] = 8;
    (void)inf_construct(&lc, lengths, 288);
    for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
    (void)inf_construct(&dc, lengths, 30);
    return inf_codes(s, &lc, &dc);
}
static int inf_dynamic(Inf *s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    Huff lc, dc;
    int nlen, ndist, ncode, index, err, b;
    b = inf_bits(s, 5); if (b < 0) return -1; nlen = b + 257;
    b = inf_bits(s, 5); if (b < 0) return -1; ndist = b + 1;
    b = inf_bits(s, 4); if (b < 0) return -1; ncode = b + 4;
    if (nlen > 286 || ndist > 30) return -1;
    for (index = 0; index < ncode; index++) {
        b = inf_bits(s, 3); if (b < 0) return -1;
        lengths[order[index]] = (short)b;
    }
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (inf_construct(&lc, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = inf_decode(s, &lc);
        if (symbol < 0) return -1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            int len = 0, rep;
            if (symbol == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                b = inf_bits(s, 2); if (b < 0) return -1; rep = 3 + b;
            } else if (symbol == 17) {
                b = inf_bits(s, 3); if (b < 0) return -1; rep = 3 + b;
            } else {
                b = inf_bits(s, 7); if (b < 0) return -1; rep = 11 + b;
            }
            if (index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    err = inf_construct(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = inf_construct(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return inf_codes(s, &lc, &dc);
}
static int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    Inf s;
    int last, type, b, err;
    memset(&s, 0, sizeof s);
    s.in = in; s.inlen = inlen; s.out = out; s.outlen = outlen;
    do {
        b = inf_bits(&s, 1); if (b < 0) return -1; last = b;
        b = inf_bits(&s, 2); if (b < 0) return -1; type = b;
        err = type == 0 ? inf_stored(&s) : type == 1 ? inf_fixed(&s) : type == 2 ? inf_dynamic(&s) : -1;
        if (err) return -1;
    } while (!last);
    return s.outpos == outlen ? 0 : -1;
}

/* ---- PNG --------------------------------------------------------------- */
typedef struct { uint32_t w, h; uint8_t depth, ctype, interlace; } Ihdr;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int32_t parse_ihdr(const uint8_t *d, size_t n, Ihdr *ih) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 33 || memcmp(d, sig, 8)) return PNG_ERR_DATA;
    if (be32(d + 8) != 13 || memcmp(d + 12, "IHDR", 4)) return PNG_ERR_DATA;
    ih->w = be32(d + 16); ih->h = be32(d + 20);
    ih->depth = d[24]; ih->ctype = d[25]; ih->interlace = d[28];
    if (!ih->w || !ih->h || ih->w > 16384 || ih->h > 16384) return PNG_ERR_DATA;
    return 0;
}
static int channels(int ctype) {
    switch (ctype) { case 0: return 1; case 2: return 3; case 3: return 1; case 4: return 2; case 6: return 4; }
    return 0;
}
static int depth_ok(int ctype, int d) {
    switch (ctype) {
    case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
    case 3: return d == 1 || d == 2 || d == 4 || d == 8;
    default: return d == 8 || d == 16;
    }
}
static void fill_info(PngImageInfo *info, const Ihdr *ih) {
    static const uint16_t cs[7] = {2, 0, 3, 4, 18, 0, 19};
    info->imageWidth = ih->w; info->imageHeight = ih->h;
    info->colorSpace = ih->ctype < 7 ? cs[ih->ctype] : 0;
    info->bitDepth = ih->depth;
    info->imageFlag = ih->interlace ? 1u : 0u;
}
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}
static int unfilter(uint8_t *raw, uint32_t h, size_t stride, size_t bpp) {
    uint32_t y;
    size_t i;
    for (y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1);
        uint8_t ft = row[0];
        const uint8_t *prev;
        row++;
        prev = y ? row - (stride + 1) : NULL;
        for (i = 0; i < stride; i++) {
            int a = i >= bpp ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int x = row[i];
            switch (ft) {
            case 0: break;
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += (a + b) / 2; break;
            case 4: x += paeth(a, b, c); break;
            default: return -1;
            }
            row[i] = (uint8_t)x;
        }
    }
    return 0;
}
static unsigned sample(const uint8_t *row, size_t idx, int depth) {
    unsigned per, byte, shift;
    if (depth == 8) return row[idx];
    if (depth == 16) return ((unsigned)row[idx * 2] << 8) | row[idx * 2 + 1];
    per = 8u / (unsigned)depth;
    byte = row[idx / per];
    shift = 8u - (unsigned)depth * (unsigned)(idx % per + 1);
    return (byte >> shift) & ((1u << depth) - 1u);
}
static unsigned scale8(unsigned s, int depth) {
    if (depth == 16) return s >> 8;
    if (depth == 8) return s;
    return s * 255u / ((1u << depth) - 1u);
}

static int32_t png_decode(const uint8_t *d, size_t n, uint8_t *out, uint32_t pitch, int bgra, uint8_t adef) {
    Ihdr ih;
    int32_t rc = parse_ihdr(d, n, &ih);
    uint8_t plte[768], palpha[256];
    uint8_t *idat = NULL, *raw = NULL;
    int nplte = 0, has_key = 0, chans, depth;
    unsigned key[3] = {0, 0, 0};
    size_t pos = 33, idat_len = 0, bpbits, stride, bpp, rawlen;
    uint32_t x, y;
    if (rc) return rc;
    if (ih.interlace) return PNG_ERR_UNSUPPORTED;
    chans = channels(ih.ctype);
    depth = ih.depth;
    if (!chans || !depth_ok(ih.ctype, depth)) return PNG_ERR_UNSUPPORTED;
    memset(plte, 0, sizeof plte);
    memset(palpha, adef, sizeof palpha);
    idat = (uint8_t *)malloc(n);
    if (!idat) return PNG_ERR_DECODE;
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos), i;
        const uint8_t *type = d + pos + 4, *cd = d + pos + 8;
        if (len > n - pos - 12) break;
        if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + idat_len, cd, len); idat_len += len;
        } else if (!memcmp(type, "PLTE", 4)) {
            nplte = len / 3 > 256 ? 256 : (int)(len / 3);
            memcpy(plte, cd, (size_t)nplte * 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (ih.ctype == 3) {
                for (i = 0; i < len && i < 256; i++) palpha[i] = cd[i];
            } else if (ih.ctype == 0 && len >= 2) {
                has_key = 1; key[0] = ((unsigned)cd[0] << 8) | cd[1];
            } else if (ih.ctype == 2 && len >= 6) {
                has_key = 1;
                for (i = 0; i < 3; i++) key[i] = ((unsigned)cd[2 * i] << 8) | cd[2 * i + 1];
            }
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    if (idat_len < 2 || (ih.ctype == 3 && nplte == 0)) { free(idat); return PNG_ERR_DATA; }
    bpbits = (size_t)chans * (size_t)depth;
    stride = ((size_t)ih.w * bpbits + 7) / 8;
    bpp = (bpbits + 7) / 8;
    rawlen = (size_t)ih.h * (stride + 1);
    raw = (uint8_t *)malloc(rawlen);
    if (!raw) { free(idat); return PNG_ERR_DECODE; }
    if (inflate_raw(idat + 2, idat_len - 2, raw, rawlen) || unfilter(raw, ih.h, stride, bpp)) {
        free(idat); free(raw); return PNG_ERR_DECODE;
    }
    for (y = 0; y < ih.h; y++) {
        const uint8_t *row = raw + (size_t)y * (stride + 1) + 1;
        uint8_t *o = out + (size_t)y * pitch;
        for (x = 0; x < ih.w; x++, o += 4) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            switch (ih.ctype) {
            case 0: {
                unsigned s = sample(row, x, depth);
                r = g = b = scale8(s, depth);
                a = (has_key && s == key[0]) ? 0 : adef;
            } break;
            case 2: {
                unsigned sr = sample(row, (size_t)x * 3, depth);
                unsigned sg = sample(row, (size_t)x * 3 + 1, depth);
                unsigned sb = sample(row, (size_t)x * 3 + 2, depth);
                r = scale8(sr, depth); g = scale8(sg, depth); b = scale8(sb, depth);
                a = (has_key && sr == key[0] && sg == key[1] && sb == key[2]) ? 0 : adef;
            } break;
            case 3: {
                unsigned idx = sample(row, x, depth);
                if ((int)idx >= nplte) idx = 0;
                r = plte[idx * 3]; g = plte[idx * 3 + 1]; b = plte[idx * 3 + 2];
                a = palpha[idx];
            } break;
            case 4:
                r = g = b = scale8(sample(row, (size_t)x * 2, depth), depth);
                a = scale8(sample(row, (size_t)x * 2 + 1, depth), depth);
                break;
            default:
                r = scale8(sample(row, (size_t)x * 4, depth), depth);
                g = scale8(sample(row, (size_t)x * 4 + 1, depth), depth);
                b = scale8(sample(row, (size_t)x * 4 + 2, depth), depth);
                a = scale8(sample(row, (size_t)x * 4 + 3, depth), depth);
                break;
            }
            o[0] = (uint8_t)(bgra ? b : r); o[1] = (uint8_t)g;
            o[2] = (uint8_t)(bgra ? r : b); o[3] = (uint8_t)a;
        }
    }
    free(idat); free(raw);
    return 0;
}

/* ---- Guest API --------------------------------------------------------- */
static char dummy_handle;

static ABI int32_t pngdec_query_memory_size(const PngCreateParam *p) {
    if (!p) return PNG_ERR_ADDR;
    return 0x4000;
}
static ABI int32_t pngdec_create(const PngCreateParam *p, void *mem, uint32_t size, void **handle) {
    (void)size;
    if (!p || !handle) return PNG_ERR_ADDR;
    *handle = mem ? mem : (void *)&dummy_handle;
    return 0;
}
static ABI int32_t pngdec_delete(void *h) { (void)h; return 0; }
static ABI int32_t pngdec_parse_header(const PngParseParam *p, PngImageInfo *info) {
    Ihdr ih;
    int32_t rc;
    if (!p || !p->pngMemAddr || !info) return PNG_ERR_ADDR;
    rc = parse_ihdr((const uint8_t *)p->pngMemAddr, p->pngMemSize, &ih);
    if (rc) return rc;
    fill_info(info, &ih);
    return 0;
}
static ABI int32_t pngdec_decode(void *h, const PngDecodeParam *p, PngImageInfo *info) {
    Ihdr ih;
    uint32_t pitch;
    int32_t rc;
    if (!h) return PNG_ERR_HANDLE;
    if (!p || !p->pngMemAddr || !p->imageMemAddr) return PNG_ERR_ADDR;
    rc = parse_ihdr((const uint8_t *)p->pngMemAddr, p->pngMemSize, &ih);
    if (rc) return rc;
    pitch = p->imagePitch ? p->imagePitch : ih.w * 4;
    if (pitch < ih.w * 4 || (uint64_t)pitch * (ih.h - 1) + (uint64_t)ih.w * 4 > p->imageMemSize)
        return PNG_ERR_SIZE;
    rc = png_decode((const uint8_t *)p->pngMemAddr, p->pngMemSize, (uint8_t *)p->imageMemAddr,
                    pitch, p->pixelFormat == 1, (uint8_t)p->alphaValue);
    if (rc) return rc;
    if (info) fill_info(info, &ih);
    return 0;
}

static const RuntimeExport exports[] = {
    {"scePngDecQueryMemorySize", pngdec_query_memory_size},
    {"scePngDecCreate", pngdec_create},
    {"scePngDecParseHeader", pngdec_parse_header},
    {"scePngDecDecode", pngdec_decode},
    {"scePngDecDelete", pngdec_delete},
};

uintptr_t runtime_pngdec_resolve(const char *nid) { return RUNTIME_LOOKUP(exports, nid); }
