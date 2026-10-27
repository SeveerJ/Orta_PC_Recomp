/* unzip.c -- see unzip.h.
 *
 * The inflater follows the structure of Mark Adler's puff.c (the reference
 * decoder in zlib's contrib): canonical Huffman tables built from code
 * lengths, decoded a bit at a time. Slow next to zlib, but small, and Setup
 * only unpacks a few tens of megabytes. No C runtime: errors are a flag,
 * not longjmp. */
#include "unzip.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const unsigned char *in;
    unsigned long inlen, incnt;
    unsigned long bitbuf;
    int bitcnt;
    unsigned char *out;
    unsigned long outlen, outcnt;
    int err;
} State;

typedef struct { short *count; short *symbol; } Huff;

static int bits(State *s, int need)
{
    unsigned long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (unsigned long)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1UL << need) - 1));
}

static int stored(State *s)
{
    unsigned len;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return 1;
    len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    if (s->in[s->incnt + 2] != (~len & 0xff) || s->in[s->incnt + 3] != ((~len >> 8) & 0xff))
        return 1;
    s->incnt += 4;
    if (s->incnt + len > s->inlen || s->outcnt + len > s->outlen) return 1;
    while (len--) s->out[s->outcnt++] = s->in[s->incnt++];
    return 0;
}

static int decode(State *s, const Huff *h)
{
    int len, code = 0, first = 0, index = 0, count;
    for (len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Returns <0 for an over-subscribed set, 0 complete, >0 incomplete. */
static int construct(Huff *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int codes(State *s, const Huff *lencode, const Huff *distcode)
{
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                     257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                     8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol;
    unsigned long len, dist;
    for (;;) {
        symbol = decode(s, lencode);
        if (symbol < 0 || s->err) return 1;
        if (symbol < 256) {
            if (s->outcnt >= s->outlen) return 1;
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) return 1;
            len = (unsigned long)(lbase[symbol] + bits(s, lext[symbol]));
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30 || s->err) return 1;
            dist = (unsigned long)(dbase[symbol] + bits(s, dext[symbol]));
            if (s->err || dist > s->outcnt || s->outcnt + len > s->outlen) return 1;
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    }
}

static int fixed(State *s)
{
    static int built;
    static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    static Huff lencode, distcode;
    if (!built) {
        short lengths[FIXLCODES];
        int symbol;
        lencode.count = lencnt; lencode.symbol = lensym;
        distcode.count = distcnt; distcode.symbol = distsym;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(State *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen, ndist, ncode, index, err;
    short lengths[MAXLCODES + MAXDCODES];
    short lencnt[MAXBITS + 1], lensym[MAXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    Huff lencode, distcode;
    lencode.count = lencnt; lencode.symbol = lensym;
    distcode.count = distcnt; distcode.symbol = distsym;

    nlen = bits(s, 5) + 257;
    ndist = bits(s, 5) + 1;
    ncode = bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return 1;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err || construct(&lencode, lengths, 19) != 0) return 1;

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode), len;
        if (symbol < 0 || s->err) return 1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            len = 0;
            if (symbol == 16) {
                if (index == 0) return 1;
                len = lengths[index - 1];
                symbol = 3 + bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + bits(s, 3);
            } else {
                symbol = 11 + bits(s, 7);
            }
            if (s->err || index + symbol > nlen + ndist) return 1;
            while (symbol--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return 1;
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return 1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return 1;
    return codes(s, &lencode, &distcode);
}

static int inflate_raw(unsigned char *out, unsigned long outlen,
                       const unsigned char *in, unsigned long inlen)
{
    State s;
    int last, type, err;
    s.in = in; s.inlen = inlen; s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.outlen = outlen; s.outcnt = 0;
    s.err = 0;
    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.err) return 1;
        err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : 1;
        if (err || s.err) return 1;
    } while (!last);
    return s.outcnt == outlen ? 0 : 1;
}

/* ── CRC-32 ─────────────────────────────────────────────────────────── */
static unsigned long crc32(const unsigned char *p, unsigned long n)
{
    static unsigned long table[256];
    unsigned long c = 0xFFFFFFFFUL;
    if (!table[1]) {
        unsigned long i, k, v;
        for (i = 0; i < 256; i++) {
            v = i;
            for (k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    }
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

/* ── zip ────────────────────────────────────────────────────────────── */
static unsigned long rd16(const unsigned char *p) { return p[0] | (unsigned long)p[1] << 8; }
static unsigned long rd32(const unsigned char *p)
{
    return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* Relative path only: no drive, no leading slash, no ".." part. */
static int safe_name(const char *n)
{
    const char *p = n;
    if (!*n || *n == '/' || *n == '\\') return 0;
    for (; *p; p++) if (*p == ':') return 0;
    p = n;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - p == 2 && p[0] == '.' && p[1] == '.') return 0;
        p = *e ? e + 1 : e;
    }
    return 1;
}

int unz_extract(const unsigned char *zip, unsigned long size, unz_put_fn put, void *ctx,
                unz_alloc_fn alloc, unz_free_fn release)
{
    unsigned long eocd, i, n, cd, pos, lim;
    if (size < 22) return UNZ_NOT_ZIP;
    /* End-of-central-directory record: the last 22 bytes plus a comment. */
    lim = size > 65557 ? size - 65557 : 0;
    for (eocd = size - 22;; eocd--) {
        if (rd32(zip + eocd) == 0x06054b50UL) break;
        if (eocd == lim) return UNZ_NOT_ZIP;
    }
    n = rd16(zip + eocd + 10);
    cd = rd32(zip + eocd + 16);
    if (cd > eocd) return UNZ_NOT_ZIP;
    pos = cd;
    for (i = 0; i < n; i++) {
        unsigned long method, flags, crc, csize, usize, nlen, xlen, clen, loff, data;
        unsigned char *buf;
        char *name;
        int is_dir, r;
        if (pos + 46 > eocd || rd32(zip + pos) != 0x02014b50UL) return UNZ_BAD_ENTRY;
        flags = rd16(zip + pos + 8);
        method = rd16(zip + pos + 10);
        crc = rd32(zip + pos + 16);
        csize = rd32(zip + pos + 20);
        usize = rd32(zip + pos + 24);
        nlen = rd16(zip + pos + 28);
        xlen = rd16(zip + pos + 30);
        clen = rd16(zip + pos + 32);
        loff = rd32(zip + pos + 42);
        if (pos + 46 + nlen > eocd) return UNZ_BAD_ENTRY;
        if (flags & 1) return UNZ_UNSUPPORTED;                    /* encrypted */
        if (csize == 0xFFFFFFFFUL || usize == 0xFFFFFFFFUL) return UNZ_UNSUPPORTED; /* zip64 */
        name = (char *)alloc(nlen + 1);
        if (!name) return UNZ_NO_MEMORY;
        {
            unsigned long k;
            for (k = 0; k < nlen; k++) name[k] = (char)zip[pos + 46 + k];
            name[nlen] = 0;
        }
        pos += 46 + nlen + xlen + clen;
        if (!safe_name(name)) { release(name); return UNZ_BAD_NAME; }
        is_dir = nlen && (name[nlen - 1] == '/' || name[nlen - 1] == '\\');
        if (loff + 30 > size || rd32(zip + loff) != 0x04034b50UL) { release(name); return UNZ_BAD_ENTRY; }
        data = loff + 30 + rd16(zip + loff + 26) + rd16(zip + loff + 28);
        if (data > size || csize > size - data) { release(name); return UNZ_BAD_ENTRY; }
        if (is_dir) {
            r = put(ctx, name, 1, 0, 0);
            release(name);
            if (!r) return UNZ_WRITE;
            continue;
        }
        if (method == 0) {
            if (csize != usize) { release(name); return UNZ_BAD_DATA; }
            if (crc32(zip + data, usize) != crc) { release(name); return UNZ_CRC; }
            r = put(ctx, name, 0, zip + data, usize);
        } else if (method == 8) {
            buf = (unsigned char *)alloc(usize ? usize : 1);
            if (!buf) { release(name); return UNZ_NO_MEMORY; }
            if (inflate_raw(buf, usize, zip + data, csize)) { release(buf); release(name); return UNZ_BAD_DATA; }
            if (crc32(buf, usize) != crc) { release(buf); release(name); return UNZ_CRC; }
            r = put(ctx, name, 0, buf, usize);
            release(buf);
        } else {
            release(name);
            return UNZ_UNSUPPORTED;
        }
        release(name);
        if (!r) return UNZ_WRITE;
    }
    return UNZ_OK;
}
