/* SPDX-License-Identifier: GPL-3.0-only */
/* Musical Worlds, the data side (PLAY MODE): the FWD1 blob check, the World load into the pattern pool, the
 * stage builder and the commit. The format: docs/design/fwd1-format.md (constants: world_fmt.h, shared with
 * tools/worldc.py); the design: docs/design/play-mode-architecture.md 2.5, 2.6.
 *
 *   wb_check     every bounds check of the format, before anything reads the blob; it never casts the blob
 *                to a struct and returns WE_OK or an error code (shown as WORLD ERROR n)
 *   world_load   check, then decode PATTERNS into the RAM pool wpool[] (stopped, no stage pending)
 *   world_stage  scene + variation -> the stage: per track preset_fill, the World's pairs, the variation, the
 *                scene, clamped to the descriptors; the whitelisted globals; the patterns (main loop)
 *   world_commit the stage into trk[] / song.g (the ISR on a boundary, Phase 11; IRQs off while stopped)
 *   world_apply  stage + commit while stopped, and the World becomes active
 *
 * Main loop, except world_commit and world_block (audio ISR). It needs core.h .. seq.c and params.c's
 * preset_fill, nothing of the UI, so the host tests build it on tests/hostsim.c. It never reads or writes the
 * user's project slots (design D6; run_tests.sh greps for it): a World's scenes are its own. */
#include "felucca_worlds.h"            /* WORLD_DATA, WORLD_INDEX, WORLD_NFACTORY (tools/gen_worlds.py) */

typedef struct {                       /* a checked blob: where its sections and records are */
    const uint8_t *b;
    uint32_t len, have;                /* have: bit per section type present */
    uint16_t off[WF_S_LAST + 1], slen[WF_S_LAST + 1];   /* payload offset (from b) and length per type */
    uint8_t cnt[WF_S_LAST + 1];
    uint8_t scale, keys;               /* META scale, KEYS track */
    uint16_t trk[WF_NTRK], pat[WF_MAX_PAT], scene[WF_NSCENE], var[WF_MAX_VARS];   /* record offsets */
} wb_ctx_t;
static wb_ctx_t wctx;                  /* the loaded World */
static uint8_t wbeat_pat = WF_NONE;    /* BEAT: the pool entry wreq_block swaps in on the next bar (WF_NONE: none) */

static const uint8_t WB_PFIXED[] = {WF_P_FIXED}, WB_PDRUM[] = {WF_P_DRUM}, WB_PSTRUCT[] = {WF_P_STRUCT};
static const uint8_t WB_GWHITE[] = {WF_G_WHITELIST}, WB_GSTRUCT[] = {WF_G_STRUCT}, WB_GNOVAR[] = {WF_G_NOVAR};
static const uint8_t WB_GMIN[] = WF_GUARD_MIN, WB_GMAX[] = WF_GUARD_MAX;
static const uint8_t WB_TRANS[] = WF_TRANSITIONS, WB_PBEATS[] = WF_PROG_BEATS;
_Static_assert(sizeof WB_GMIN == WF_G_RESERVED - WF_G_POLY && sizeof WB_GMAX == sizeof WB_GMIN, "GUARD fields");
_Static_assert(WF_NTRK == NTRK && WF_TRK_DRUM == TRK_DRUM && WF_NLANES == DRUM_LANES && WF_MAX_LEN_STEPS == NSTEP,
               "world_fmt.h matches core.h");

#define WB_NEED(x, e) do { if (!(x)) return (e); } while (0)
#define WB_HAS(list, v) wb_has(list, sizeof list, v)

static uint32_t wb_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t wb_u32(const uint8_t *p) { return wb_u16(p) | wb_u16(p + 2) << 16; }
static int wb_has(const uint8_t *l, uint32_t n, uint32_t v)
{
    while (n)
        if (l[--n] == v)
            return 1;
    return 0;
}

static uint32_t wb_crc_run(uint32_t c, const uint8_t *b, uint32_t n)   /* CRC-32 (zlib), as storage.c st_crc32 */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static uint32_t wb_crc(const uint8_t *b, uint32_t L)   /* the blob's CRC: every byte but the CRC field */
{
    return ~wb_crc_run(wb_crc_run(0xFFFFFFFFu, b, WF_CRC_AT), b + WF_CRC_FROM, L - WF_CRC_FROM);
}

/* a NUL-padded string of n bytes (the last one NUL): characters lo..hi up to the first NUL, NULs after it;
 * hi 255 = printable Latin-1 (FONT_S). need: at least one character */
static int wb_str(const uint8_t *s, uint32_t n, uint32_t lo, uint32_t hi, int need)
{
    uint32_t i, end = 0;
    for (i = 0; i < n; i++) {
        uint32_t ch = s[i];
        if (end || !ch) {
            if (ch)
                return 0;
            end = 1;
        } else if (ch < lo || ch > hi || (hi == 255u && ch >= 127u && ch < 160u)) {
            return 0;
        }
    }
    return end && (!need || s[0]);
}

/* track parameter id on track t; strict: also not structural (scenes, variations, macro / rule / guard targets) */
static int wb_pid_ok(const wb_ctx_t *c, uint32_t t, uint32_t id, int strict)
{
    if (id >= P_COUNT || WB_HAS(WB_PFIXED, id) || (t == TRK_DRUM && !WB_HAS(WB_PDRUM, id)))
        return 0;
    if (!strict)
        return 1;
    if (WB_HAS(WB_PSTRUCT, id))
        return 0;
    if (t != TRK_DRUM && id >= P_E0) {
        uint32_t e = c->b[c->trk[t] + 1u], k = id - P_E0;
        if (ENGINES[e]->edit[k].fmt == F_ENUM && !(e == WF_ENUM_OK_ENGINE && k == WF_ENUM_OK_PARAM))
            return 0;
    }
    return 1;
}

static int wb_spairs(const wb_ctx_t *c, const uint8_t *p, uint32_t n, int var)   /* scoped pairs of a scene / variation */
{
    for (; n; n--, p += WF_SPAIR) {
        if (p[0] == WF_SCOPE_G) {
            if (!WB_HAS(WB_GWHITE, p[1]) || (var && WB_HAS(WB_GNOVAR, p[1])))
                return 0;
        } else if (p[0] >= NTRK || !wb_pid_ok(c, p[0], p[1], 1)) {
            return 0;
        }
    }
    return 1;
}

static int wb_target(const wb_ctx_t *c, uint32_t tg, uint32_t id)   /* MAPS / RULES / GUARD target byte + id */
{
    uint32_t kind = tg >> 5, m = tg & WF_TMASK, t;
    if (kind == WF_K_GLOBAL)
        return !m && WB_HAS(WB_GWHITE, id) && !WB_HAS(WB_GSTRUCT, id);
    if (!m || m >> NTRK)
        return 0;
    if (kind == WF_K_PARAM) {
        if (id >= P_E0 && (m & (m - 1u)))
            return 0;                                     /* an engine parameter: one track */
        for (t = 0; t < NTRK; t++)
            if ((m >> t & 1u) && !wb_pid_ok(c, t, id, 1))
                return 0;
        return 1;
    }
    if (m >> TRK_DRUM & 1u)
        return 0;
    if (kind == WF_K_ROLE)
        return id < WF_NEROLES;
    return (kind == WF_K_BRIGHT || kind == WF_K_SHAPE) && !id;
}

static int wb_pat_is(const wb_ctx_t *c, uint32_t i, int drum)   /* WF_NONE, or a pattern of that kind */
{
    if (i == WF_NONE)
        return 1;
    return i < c->cnt[WF_S_PATTERNS] && !(c->b[c->pat[i]] & WF_PAT_DRUM) == !drum;
}

static int wb_synth(const uint8_t *d, uint32_t nb, uint32_t len)   /* a synth pattern's record stream */
{
    uint32_t i = 0, c = 0, prev = 0;
    while (i < nb) {
        uint32_t k = d[i] >> 6, a = d[i] & 63u, nf, n;
        i++;
        if (k == WF_R_END)
            return a || i != nb ? WE_PATTERN : WE_OK;
        if (k != WF_R_NOTE) {
            WB_NEED(a && c + a <= len, WE_PATTERN);
            c += a;
            continue;
        }
        c += a;
        WB_NEED(c < len && i < nb, WE_PATTERN);
        nf = d[i++];
        n = nf & WF_NF_N;
        WB_NEED(n <= 4u && (n || prev) && i + n <= nb, WE_PATTERN);
        for (; n; n--)
            WB_NEED(d[i++] <= 127u, WE_NOTE);
        n = (nf & WF_NF_LVLRAT ? 2u : 0u) + (nf & WF_NF_VEL ? 1u : 0u) + (nf & WF_NF_MICRO ? 1u : 0u);
        WB_NEED(i + n <= nb, WE_PATTERN);
        i += nf & WF_NF_LVLRAT ? 2u : 0u;
        if (nf & WF_NF_VEL)
            WB_NEED(d[i] >= 1u && d[i++] <= 127u, WE_PATTERN);
        if (nf & WF_NF_MICRO)
            WB_NEED(d[i] >= 1u && d[i++] <= 7u, WE_PATTERN);
        prev = 1;
        c++;
    }
    return WE_OK;
}

static int wb_drum(const uint8_t *d, uint32_t nb, uint32_t len)   /* a drum pattern: lanes ascending, bitmaps */
{
    uint32_t i = 1, l, k, nl, lane, last = 0, bytes = len / 8u;
    WB_NEED(nb >= 1u && d[0] <= WF_NLANES, WE_PATTERN);
    nl = d[0];
    for (l = 0; l < nl; l++) {
        uint32_t lb, hits = 0, m;
        WB_NEED(i < nb, WE_PATTERN);
        lb = d[i++];
        lane = lb & WF_DL_LANE;
        WB_NEED(!(lb & ~(WF_DL_LANE | WF_DL_LVL | WF_DL_RAT)) && (!l || lane > last) && i + bytes <= nb, WE_PATTERN);
        last = lane;
        for (k = 0; k < bytes; k++, i++)
            for (m = d[i]; m; m &= m - 1u)
                hits++;
        m = (2u * hits + 7u) / 8u;
        m *= (lb & WF_DL_LVL ? 1u : 0u) + (lb & WF_DL_RAT ? 1u : 0u);
        WB_NEED(i + m <= nb, WE_PATTERN);
        i += m;
    }
    return i == nb ? WE_OK : WE_PATTERN;
}

/* the whole blob b (n bytes available): WE_OK and c filled, or the first error */
static int wb_check(const uint8_t *b, uint32_t n, wb_ctx_t *c)
{
    uint32_t L, ns, i, k, t, off, o, end, prev = 0;
    const uint8_t *p, *r;
    memset(c, 0, sizeof *c);
    WB_NEED(b && n >= WF_HDR, WE_SIZE);
    WB_NEED(b[0] == 'F' && b[1] == 'W' && b[2] == 'D' && b[3] == '1', WE_MAGIC);
    WB_NEED(b[4] >= 1u && b[4] <= WF_VERSION, WE_VERSION);
    WB_NEED(!(b[5] & ~WF_F_KNOWN), WE_FLAGS);
    L = wb_u16(b + 6);
    WB_NEED(L >= WF_MIN_LEN && L <= WF_MAX_LEN && L <= n, WE_SIZE);
    WB_NEED(wb_crc(b, L) == wb_u32(b + WF_CRC_AT), WE_CRC);
    ns = b[16];
    WB_NEED(ns >= 1u && ns <= WF_MAX_SEC && !b[17] && !b[18] && !b[19], WE_HEADER);
    off = WF_HDR + WF_SECENT * ns;
    WB_NEED(off <= L, WE_TABLE);
    for (i = 0; i < ns; i++) {                            /* the table: types ascending, payloads back to back */
        p = b + WF_HDR + WF_SECENT * i;
        WB_NEED(p[0] > prev && p[0] <= WF_S_LAST, WE_SECTION);
        prev = p[0];
        k = wb_u16(p + 2);
        WB_NEED(off + k <= L, WE_TABLE);
        c->off[prev] = (uint16_t)off;
        c->slen[prev] = (uint16_t)k;
        c->cnt[prev] = p[1];
        c->have |= 1u << prev;
        off += k;
    }
    WB_NEED(off == L, WE_TABLE);
    WB_NEED((c->have & WF_S_REQUIRED) == WF_S_REQUIRED, WE_MISSING);
    c->b = b;
    c->len = L;

    /* META: name, category, blurb; bpm min max root scale swing */
    p = b + c->off[WF_S_META];
    WB_NEED(c->cnt[WF_S_META] == 1u && c->slen[WF_S_META] == WF_META_LEN, WE_LENGTH);
    WB_NEED(wb_str(p, WF_NAME_LEN, WF_CH_LO, WF_CH_HI, 1) && wb_str(p + WF_NAME_LEN, WF_CAT_LEN, WF_CH_LO, WF_CH_HI, 0) &&
            wb_str(p + WF_NAME_LEN + WF_CAT_LEN, WF_BLURB_LEN, 32, 255, 0), WE_STRING);
    p += WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN;
    WB_NEED(p[1] >= WF_BPM_MIN && p[2] <= WF_BPM_MAX && p[1] <= p[0] && p[0] <= p[2] && p[3] < 12u &&
            p[4] < NSCALES && p[5] <= WF_SWING_MAX, WE_META);
    c->scale = p[4];

    /* TRACKS: 4 x {role, engine, preset / kit, register, npairs, pairs} */
    p = b + c->off[WF_S_TRACKS];
    end = c->slen[WF_S_TRACKS];
    WB_NEED(c->cnt[WF_S_TRACKS] == WF_NTRK, WE_COUNT);
    for (o = 0, t = 0; t < WF_NTRK; t++) {
        r = p + o;
        WB_NEED(o + WF_TRACK_HDR <= end, WE_LENGTH);
        WB_NEED(o + WF_TRACK_HDR + WF_PAIR * r[4] <= end, WE_LENGTH);
        WB_NEED(r[4] <= WF_MAX_PAIRS, WE_COUNT);
        if (t == TRK_DRUM)
            WB_NEED(r[0] == WF_ROLE_DRUMS && r[1] == WF_ENGINE_DRUMS && r[2] < DRUM_KITS && !r[3], WE_TRACK);
        else
            WB_NEED(r[0] < WF_NROLES && r[0] != WF_ROLE_DRUMS && r[1] < NENGINES && r[2] < ENGINES[r[1]]->npresets &&
                    r[3] <= 127u, WE_TRACK);
        c->trk[t] = (uint16_t)(r - b);
        for (k = 0; k < r[4]; k++)
            WB_NEED(wb_pid_ok(c, t, r[WF_TRACK_HDR + WF_PAIR * k], 0), WE_PARAM);
        o += WF_TRACK_HDR + WF_PAIR * r[4];
    }
    WB_NEED(o == end, WE_LENGTH);

    /* GLOBALS: {G id, value}, whitelist only */
    if (c->have >> WF_S_GLOBALS & 1u) {
        k = c->cnt[WF_S_GLOBALS];
        p = b + c->off[WF_S_GLOBALS];
        WB_NEED(k >= 1u && k <= WF_MAX_GLOBALS, WE_COUNT);
        WB_NEED(c->slen[WF_S_GLOBALS] == WF_PAIR * k, WE_LENGTH);
        for (i = 0; i < k; i++)
            WB_NEED(WB_HAS(WB_GWHITE, p[WF_PAIR * i]), WE_PARAM);
    }

    /* PATTERNS: {kind_div, len, u16 nbytes, data} */
    if (c->have >> WF_S_PATTERNS & 1u) {
        k = c->cnt[WF_S_PATTERNS];
        p = b + c->off[WF_S_PATTERNS];
        end = c->slen[WF_S_PATTERNS];
        WB_NEED(k >= 1u && k <= WF_MAX_PAT, WE_COUNT);
        for (o = 0, i = 0; i < k; i++) {
            uint32_t kd, len, nb;
            int rc;
            r = p + o;
            WB_NEED(o + WF_PAT_HDR <= end, WE_LENGTH);
            kd = r[0];
            len = r[1];
            nb = wb_u16(r + 2);
            WB_NEED(o + WF_PAT_HDR + nb <= end, WE_LENGTH);
            WB_NEED(!(kd & ~(uint32_t)(WF_PAT_DRUM | WF_PAT_DIVMASK)) && (kd & WF_PAT_DIVMASK) < WF_NDIV && len >= 1u &&
                    len <= NSTEP, WE_PATTERN);
            if (kd & WF_PAT_DRUM) {
                WB_NEED((kd & WF_PAT_DIVMASK) == WF_DIV_16 && (len == 16u || len == 32u || len == 64u), WE_PATTERN);
                rc = wb_drum(r + WF_PAT_HDR, nb, len);
            } else {
                rc = wb_synth(r + WF_PAT_HDR, nb, len);
            }
            if (rc)
                return rc;
            c->pat[i] = (uint16_t)(r - b);
            o += WF_PAT_HDR + nb;
        }
        WB_NEED(o == end, WE_LENGTH);
    }

    /* PROGS: {nchords, {root << 4 | quality, beats}} */
    k = c->cnt[WF_S_PROGS];
    p = b + c->off[WF_S_PROGS];
    end = c->slen[WF_S_PROGS];
    WB_NEED(k >= 1u && k <= WF_MAX_PROG, WE_COUNT);
    for (o = 0, i = 0; i < k; i++) {
        uint32_t nc, beats = 0;
        r = p + o;
        WB_NEED(o + WF_PROG_HDR <= end, WE_LENGTH);
        nc = r[0];
        WB_NEED(o + WF_PROG_HDR + 2u * nc <= end, WE_LENGTH);
        WB_NEED(nc >= 1u && nc <= WF_MAX_CHORDS, WE_PROG);
        for (t = 0; t < nc; t++) {
            WB_NEED((r[1u + 2u * t] >> 4) < 12u && r[2u + 2u * t] >= 1u && r[2u + 2u * t] <= WF_CHORD_BEATS_MAX, WE_PROG);
            beats += r[2u + 2u * t];
        }
        WB_NEED(WB_HAS(WB_PBEATS, beats), WE_PROG);
        o += WF_PROG_HDR + 2u * nc;
    }
    WB_NEED(o == end, WE_LENGTH);

    /* CURVES: LUT9, from 0 to 255 */
    if (c->have >> WF_S_CURVES & 1u) {
        k = c->cnt[WF_S_CURVES];
        p = b + c->off[WF_S_CURVES];
        WB_NEED(k >= 1u && k <= WF_MAX_CURVES, WE_COUNT);
        WB_NEED(c->slen[WF_S_CURVES] == WF_CURVE_LEN * k, WE_LENGTH);
        for (i = 0; i < k; i++)
            WB_NEED(!p[WF_CURVE_LEN * i] && p[WF_CURVE_LEN * i + 8u] == 255u, WE_CURVE);
    }

    /* ENERGY: {u16 density_lanes, nbands, bands {from, layers | fills | ratchets, lanes, dens, play[3]}} */
    if (c->have >> WF_S_ENERGY & 1u) {
        k = c->cnt[WF_S_ENERGY];
        p = b + c->off[WF_S_ENERGY];
        end = c->slen[WF_S_ENERGY];
        WB_NEED(k >= 1u && k <= WF_MAX_ENERGY, WE_COUNT);
        for (o = 0, i = 0; i < k; i++) {
            uint32_t nb;
            r = p + o;
            WB_NEED(o + WF_ENERGY_HDR <= end, WE_LENGTH);
            nb = r[2];
            WB_NEED(o + WF_ENERGY_HDR + WF_BAND_LEN * nb <= end, WE_LENGTH);
            WB_NEED(nb >= 1u && nb <= WF_MAX_BANDS, WE_ENERGY);
            for (t = 0; t < nb; t++) {
                const uint8_t *bd = r + WF_ENERGY_HDR + WF_BAND_LEN * t;
                WB_NEED(bd[0] <= WF_UNIT && (t ? bd[0] > bd[(int32_t)0 - WF_BAND_LEN] : !bd[0]) &&
                        !(bd[1] & ~(uint32_t)(WF_B_LAYERS | WF_B_FILLS | WF_B_RATCHETS)), WE_ENERGY);
            }
            o += WF_ENERGY_HDR + WF_BAND_LEN * nb;
        }
        WB_NEED(o == end, WE_LENGTH);
    }

    /* KEYS: trk, mode, u16 melody_mask, white, black, tonic, loop_pat */
    p = b + c->off[WF_S_KEYS];
    WB_NEED(c->cnt[WF_S_KEYS] == 1u && c->slen[WF_S_KEYS] == WF_KEYS_LEN, WE_LENGTH);
    k = wb_u16(p + 2);
    WB_NEED(p[0] < NPART && p[1] < WF_NKMODES && (k & 1u) && !(k & ~(uint32_t)SCALE_MASK[c->scale]) && p[4] < WF_NWB &&
            p[5] < WF_NWB && p[6] <= 127u, WE_KEYS);
    WB_NEED(wb_pat_is(c, p[7], 0), WE_INDEX);
    c->keys = p[0];

    /* SCENES: 4 x {name[11] role prog energy transition fill pat[3] beat[4] npairs, pairs} */
    p = b + c->off[WF_S_SCENES];
    end = c->slen[WF_S_SCENES];
    WB_NEED(c->cnt[WF_S_SCENES] == WF_NSCENE, WE_COUNT);
    for (o = 0, i = 0; i < WF_NSCENE; i++) {
        r = p + o;
        WB_NEED(o + WF_SCENE_HDR <= end, WE_LENGTH);
        WB_NEED(o + WF_SCENE_HDR + WF_SPAIR * r[23] <= end, WE_LENGTH);
        WB_NEED(wb_str(r, WF_LABEL_LEN, WF_CH_LO, WF_CH_HI, 1), WE_STRING);
        WB_NEED(r[11] < WF_NSROLES && WB_HAS(WB_TRANS, r[14]), WE_SCENE);
        WB_NEED(r[12] < c->cnt[WF_S_PROGS] && (r[13] == WF_NONE || r[13] < c->cnt[WF_S_ENERGY]) && wb_pat_is(c, r[15], 1),
                WE_INDEX);
        for (t = 0; t < NPART; t++) {
            WB_NEED(wb_pat_is(c, r[16u + t], 0), WE_INDEX);
            WB_NEED(t != c->keys || r[16u + t] == WF_NONE, WE_SCENE);
        }
        for (t = 0; t < WF_NBEATS; t++)
            WB_NEED(wb_pat_is(c, r[19u + t], 1), WE_INDEX);
        WB_NEED(r[23] <= WF_MAX_PAIRS, WE_COUNT);
        WB_NEED(wb_spairs(c, r + WF_SCENE_HDR, r[23], 0), WE_PARAM);
        c->scene[i] = (uint16_t)(r - b);
        o += WF_SCENE_HDR + WF_SPAIR * r[23];
    }
    WB_NEED(o == end, WE_LENGTH);

    /* VARS: {name[11], i8 bias, nsound {trk, preset}, nswap {from, to}, npairs, pairs}; the first is empty */
    k = c->cnt[WF_S_VARS];
    p = b + c->off[WF_S_VARS];
    end = c->slen[WF_S_VARS];
    WB_NEED(k >= 1u && k <= WF_MAX_VARS, WE_COUNT);
    for (o = 0, i = 0; i < k; i++) {
        uint32_t na, nb, np, q, j;
        int32_t bias;
        r = p + o;
        WB_NEED(o + WF_VAR_HDR - 2u <= end, WE_LENGTH);
        WB_NEED(wb_str(r, WF_LABEL_LEN, WF_CH_LO, WF_CH_HI, 1), WE_STRING);
        bias = (int8_t)r[11];
        na = r[12];
        q = o + WF_VAR_HDR - 2u;
        WB_NEED(bias >= -WF_BIAS_MAX && bias <= WF_BIAS_MAX && na <= NTRK, WE_VAR);
        WB_NEED(q + 2u * na + 1u <= end, WE_LENGTH);
        for (j = 0; j < na; j++) {
            uint32_t tt = p[q + 2u * j], pr = p[q + 2u * j + 1u];
            WB_NEED(tt < NTRK && pr < (tt == TRK_DRUM ? DRUM_KITS : ENGINES[b[c->trk[tt] + 1u]]->npresets), WE_VAR);
        }
        q += 2u * na;
        nb = p[q++];
        WB_NEED(nb <= WF_MAX_PAT, WE_COUNT);
        WB_NEED(q + 2u * nb + 1u <= end, WE_LENGTH);
        for (j = 0; j < 2u * nb; j++)
            WB_NEED(p[q + j] != WF_NONE && wb_pat_is(c, p[q + j], 0), WE_INDEX);
        q += 2u * nb;
        np = p[q++];
        WB_NEED(np <= WF_MAX_PAIRS, WE_COUNT);
        WB_NEED(q + WF_SPAIR * np <= end, WE_LENGTH);
        WB_NEED(wb_spairs(c, p + q, np, 1), WE_PARAM);
        WB_NEED(i || (!na && !nb && !np && !bias), WE_VAR);
        c->var[i] = (uint16_t)(r - b);
        o = q + WF_SPAIR * np;
    }
    WB_NEED(o == end, WE_LENGTH);

    /* MAPS: {ctl, target, id, class << 6 | curve, i8 min, i8 max} */
    if (c->have >> WF_S_MAPS & 1u) {
        k = c->cnt[WF_S_MAPS];
        p = b + c->off[WF_S_MAPS];
        WB_NEED(k >= 1u && k <= WF_MAX_MAPS, WE_COUNT);
        WB_NEED(c->slen[WF_S_MAPS] == WF_MAP_LEN * k, WE_LENGTH);
        for (i = 0; i < k; i++, p += WF_MAP_LEN) {
            uint32_t cv = p[3] & WF_CURVE_MASK;
            WB_NEED(p[0] < WF_NCTL && wb_target(c, p[1], p[2]), WE_MAP);
            WB_NEED(cv < WF_NBUILTIN_CURVES || (cv >= WF_CURVE_CUSTOM && cv < WF_CURVE_CUSTOM + c->cnt[WF_S_CURVES]),
                    WE_INDEX);
        }
    }

    /* RULES: {a << 4 | b, ta, tb, nact, {target, id, i8 add, 0}} */
    if (c->have >> WF_S_RULES & 1u) {
        k = c->cnt[WF_S_RULES];
        p = b + c->off[WF_S_RULES];
        end = c->slen[WF_S_RULES];
        WB_NEED(k >= 1u && k <= WF_MAX_RULES, WE_COUNT);
        for (o = 0, i = 0; i < k; i++) {
            r = p + o;
            WB_NEED(o + WF_RULE_HDR <= end, WE_LENGTH);
            WB_NEED(o + WF_RULE_HDR + WF_ACT_LEN * r[3] <= end, WE_LENGTH);
            WB_NEED(r[1] <= WF_THRESH_MAX && r[2] <= WF_THRESH_MAX && r[3] >= 1u && r[3] <= WF_MAX_ACTS, WE_RULE);
            for (t = 0; t < r[3]; t++) {
                const uint8_t *a = r + WF_RULE_HDR + WF_ACT_LEN * t;
                WB_NEED(wb_target(c, a[0], a[1]) && !a[3], WE_RULE);
            }
            o += WF_RULE_HDR + WF_ACT_LEN * r[3];
        }
        WB_NEED(o == end, WE_LENGTH);
    }

    /* GUARD: 32 fixed bytes (WF_NONE = unset), then {target, id, i8 lo, i8 hi} x count, then the combinations
     * {target, id, i8 over} x 3 (the last: the capped target, its cap) x fixed byte WF_G_COMBOS (WF_NONE: none) */
    if (c->have >> WF_S_GUARD & 1u) {
        uint32_t nc;
        k = c->cnt[WF_S_GUARD];
        p = b + c->off[WF_S_GUARD];
        WB_NEED(k <= WF_MAX_GRANGES, WE_COUNT);
        WB_NEED(c->slen[WF_S_GUARD] >= WF_GUARD_FIX, WE_LENGTH);
        nc = p[WF_G_COMBOS] == WF_NONE ? 0u : p[WF_G_COMBOS];   /* (0..8: checked with the fixed fields below) */
        WB_NEED(c->slen[WF_S_GUARD] == WF_GUARD_FIX + WF_GRANGE_LEN * k + WF_COMBO_LEN * nc, WE_LENGTH);
        for (t = 0; t < NPART; t++)
            WB_NEED((p[2u * t] == WF_NONE && p[2u * t + 1u] == WF_NONE) || (p[2u * t] <= p[2u * t + 1u] && p[2u * t + 1u] <= 127u),
                    WE_GUARD);
        for (i = WF_G_POLY; i < WF_G_RESERVED; i++)
            WB_NEED(p[i] == WF_NONE || (p[i] >= WB_GMIN[i - WF_G_POLY] && p[i] <= WB_GMAX[i - WF_G_POLY]), WE_GUARD);
        for (; i < WF_GUARD_FIX; i++)
            WB_NEED(p[i] == WF_NONE, WE_GUARD);
        for (p += WF_GUARD_FIX, i = 0; i < k; i++, p += WF_GRANGE_LEN)
            WB_NEED(wb_target(c, p[0], p[1]) && (int8_t)p[2] <= (int8_t)p[3], WE_GUARD);
        for (i = 0; i < 3u * nc; i++, p += 3)            /* (a combination's targets: parameters, roles, globals) */
            WB_NEED(wb_target(c, p[0], p[1]) && (p[0] >> 5 == WF_K_PARAM || p[0] >> 5 == WF_K_ROLE ||
                                                p[0] >> 5 == WF_K_GLOBAL), WE_GUARD);
    }

    /* DEFAULTS: scene var ctl[4] pulse beat shape[4] move[4] */
    p = b + c->off[WF_S_DEFAULTS];
    WB_NEED(c->cnt[WF_S_DEFAULTS] == 1u && c->slen[WF_S_DEFAULTS] == WF_DEFAULTS_LEN, WE_LENGTH);
    WB_NEED(p[0] < WF_NSCENE && p[1] < c->cnt[WF_S_VARS] && p[6] < WF_NPULSE && p[7] < WF_NBEATS, WE_DEFAULTS);
    for (i = 2; i < WF_DEFAULTS_LEN; i++)
        WB_NEED(i == 6u || i == 7u || p[i] <= WF_UNIT, WE_DEFAULTS);
    return WE_OK;
}

/* ------------------------------------------------------------------- load --- */
/* a checked PATTERNS record -> 64 steps, as SLOOP keeps them (REST steps as steps_clear, TIE steps as rec_hold) */
static void wb_pattern(const uint8_t *r, wpat_t *w)
{
    uint32_t len = r[1], nb = wb_u16(r + 2), i = 0, c = 0, k, n = 0;
    const uint8_t *d = r + WF_PAT_HDR;
    uint8_t notes[4] = {0, 0, 0, 0};
    memset(w, 0, sizeof *w);
    if (r[0] & WF_PAT_DRUM) {
        uint32_t nl = d[0], l, bytes = len / 8u;
        for (i = 1, l = 0; l < nl; l++) {
            uint32_t lb = d[i++], lane = lb & WF_DL_LANE, s, h = 0, nh = 0;
            const uint8_t *bm = d + i, *lv, *rt;
            i += bytes;
            for (s = 0; s < len; s++)
                nh += bm[s >> 3] >> (s & 7u) & 1u;
            lv = lb & WF_DL_LVL ? d + i : 0;
            i += lv ? (2u * nh + 7u) / 8u : 0u;
            rt = lb & WF_DL_RAT ? d + i : 0;
            i += rt ? (2u * nh + 7u) / 8u : 0u;
            for (s = 0; s < len; s++)
                if (bm[s >> 3] >> (s & 7u) & 1u) {
                    dstep_set(&w->dstep[s], lane, lv ? lv[h >> 2] >> (2u * (h & 3u)) & 3u : LV_NORM,
                              rt ? rt[h >> 2] >> (2u * (h & 3u)) & 3u : 0u);
                    h++;
                }
        }
        return;
    }
    for (k = 0; k < NSTEP; k++)
        w->step[k].time = ST_REST;
    while (i < nb && c < len) {
        uint32_t kind = d[i] >> 6, a = d[i] & 63u, nf;
        step_t *s;
        i++;
        if (kind == WF_R_END)
            break;
        if (kind != WF_R_NOTE) {
            for (k = 0; k < a && c < len; k++, c++)
                if (kind == WF_R_TIE)
                    w->step[c].time = ST_TIE;
            continue;
        }
        c += a;
        if (c >= len)
            break;
        nf = d[i++];
        if (nf & WF_NF_N)
            for (n = nf & WF_NF_N, k = 0; k < n; k++)
                notes[k] = d[i++];
        s = &w->step[c++];
        s->time = ST_NOTE;
        s->n = (uint8_t)n;
        for (k = 0; k < n; k++)
            s->note[k] = notes[k];
        s->flags = (uint8_t)((nf & WF_NF_ACCENT ? SF_ACCENT : 0u) | (nf & WF_NF_SLIDE ? SF_SLIDE : 0u));
        if (nf & WF_NF_LVLRAT) {
            s->lvl = d[i];
            s->rat = d[i + 1u];
            i += 2;
        }
        if (nf & WF_NF_VEL)
            s->vel = d[i++];
        if (nf & WF_NF_MICRO)
            s->flags |= (uint8_t)((d[i++] & 7u) << 2);    /* step flags b2-4: micro-timing (Phase 10) */
    }
}

/* the loaded World's MAPS, CURVES, RULES and DEFAULTS to the macros (keep: the player's positions stay), its
 * GUARD to the guardrails (guard.c) */
static void wb_macros(int keep)
{
#define WB_SEC(t) (wctx.have >> (t) & 1u ? wctx.b + wctx.off[t] : 0), (wctx.have >> (t) & 1u ? wctx.cnt[t] : 0u)
    guard_load(WB_SEC(WF_S_GUARD));
    macro_load(WB_SEC(WF_S_MAPS), WB_SEC(WF_S_CURVES), WB_SEC(WF_S_RULES), wctx.b + wctx.off[WF_S_DEFAULTS], keep);
#undef WB_SEC
}

/* check blob b and make it the loaded World: its patterns into the pool. The next commit switches World.
 * Not while a World plays (Phase 11 switches on the bar) or a stage is pending. */
static int world_load(const uint8_t *b, uint32_t n)
{
    wb_ctx_t c;
    uint32_t i;
    int rc;
    if ((wrt.active && (song.playing || transport_req)) || wst.st != WST_FREE)
        return WE_BUSY;
    rc = wb_check(b, n, &c);
    if (rc)
        return rc;
    wctx = c;
    for (i = 0; i < c.cnt[WF_S_PATTERNS]; i++)
        wb_pattern(c.b + c.pat[i], &wpool[i]);
    for (i = 0; i < NTRK; i++)
        wrt.cur_pat[i] = WF_NONE;                         /* (the pool is new: nothing to write back into it) */
    wbeat_pat = WF_NONE;
    wrt.keys_trk = c.keys;
    wrt.beat = c.b[c.off[WF_S_DEFAULTS] + 7u];
    wrt.id = wb_u32(b + 8);
    wrt.loaded = 1;
    wst.sw = 1;
    wb_macros(0);                                         /* its mappings and default positions (macro.c) */
    return WE_OK;
}

/* ------------------------------------------------------------------ stage --- */
/* the PULSE presets of the keys track's arp group at a World switch (design 9.2; Phase 13 owns PULSE):
 * AMODE ARATE AOCT AGATE (the rest of the group at its defaults, AHOLD off) */
static const uint8_t WB_PULSE[WF_NPULSE][4] = {{0, 2, 1, 64}, {1, 1, 1, 60}, {3, 2, 1, 50}, {1, 2, 2, 35}};

static void ws_spairs(const uint8_t *q, uint32_t n, uint32_t scope, int16_t *dst)   /* the pairs of one scope */
{
    for (; n; n--, q += WF_SPAIR)
        if (q[0] == scope)
            dst[q[1]] = (int8_t)q[2];
}

/* the stage's harmony and Smart Keys (design 3, 4; harmony.c, smartkeys.c): progression prog of the World in its
 * key, the KEYS section and the GUARD range, polyphony and avoid policy, built into their staged buffers; the
 * commit flips them in with the rest */
static void ws_keys(uint32_t prog)
{
    const uint8_t *meta = wctx.b + wctx.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN;
    const uint8_t *pg = wctx.b + wctx.off[WF_S_PROGS];
    const uint8_t *gd = wctx.have >> WF_S_GUARD & 1u ? wctx.b + wctx.off[WF_S_GUARD] : 0;
    while (prog--)
        pg += WF_PROG_HDR + 2u * pg[0];
    harm_stage(pg, meta[3], meta[4], gd ? gd[WF_G_AVOID] : WF_NONE);
    sk_stage(wctx.b + wctx.off[WF_S_KEYS], gd);
}

/* scene + variation of the loaded World -> the stage (composition order: docs/design/fwd1-format.md, Staging) */
static int world_stage(uint32_t scene, uint32_t var)
{
    const uint8_t *meta, *sc, *vr, *sw, *vp, *def;
    uint32_t t, i, na, nb, kt = wrt.keys_trk;
    if (!wrt.loaded || scene >= WF_NSCENE || var >= wctx.cnt[WF_S_VARS])
        return WE_STATE;
    if (wst.st == WST_READY) {                            /* a newer request replaces one not yet committed */
        fm1_irq_off();
        if (wst.st == WST_READY)
            wst.st = WST_FREE;
        fm1_irq_on();
    }
    if (wst.st != WST_FREE)
        return WE_BUSY;
    meta = wctx.b + wctx.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN;   /* bpm min max root scale swing */
    def = wctx.b + wctx.off[WF_S_DEFAULTS];
    sc = wctx.b + wctx.scene[scene];
    vr = wctx.b + wctx.var[var] + WF_LABEL_LEN + 1u;      /* nsound */
    na = vr[0];
    sw = vr + 2u + 2u * na;                               /* the swaps (count at sw[-1]) */
    nb = sw[-1];
    vp = sw + 2u * nb;                                    /* npairs, pairs */
    wst.scene = (uint8_t)scene;
    wst.var = (uint8_t)var;
    wst.prog = sc[12];
    wst.energy = sc[13];
    wst.fill = sc[15];
    wst.bpm = meta[0];
    for (t = 0; t < NTRK; t++) {
        const uint8_t *tr = wctx.b + wctx.trk[t];
        int16_t *p = wst.p[t];
        uint32_t e = t == TRK_DRUM ? 0u : tr[1], pre = tr[2], pat;
        for (i = 0; i < na; i++)                          /* the variation's sound (the drum track: its kit) */
            if (vr[1u + 2u * i] == t)
                pre = vr[2u + 2u * i];
        for (i = 0; i < P_E0; i++)
            p[i] = TP[i].def;
        if (t == TRK_DRUM) {
            for (i = P_E0; i < P_COUNT; i++)
                p[i] = 0;
            p[P_E0] = (int16_t)pre;
        } else {
            for (i = 0; i < 8u; i++)
                p[P_E0 + i] = ENGINES[e]->edit[i].def;
            preset_fill(p, ENGINES[e], pre);              /* the factory preset, as the PRESETS knob loads it */
            p[P_ROOT] = meta[3];
            p[P_SCALE] = meta[4];
        }
        for (i = 0; i < tr[4]; i++)                       /* the World's sound */
            p[tr[WF_TRACK_HDR + WF_PAIR * i]] = (int8_t)tr[WF_TRACK_HDR + WF_PAIR * i + 1u];
        ws_spairs(vp + 1, vp[0], t, p);                   /* the variation */
        ws_spairs(sc + WF_SCENE_HDR, sc[23], t, p);       /* the scene */
        /* the pattern: the scene's (the drum track: the BEAT's, else the GROOVE), through the variation's swaps */
        pat = t < NPART ? sc[16u + t] : sc[19u + wrt.beat] != WF_NONE ? sc[19u + wrt.beat] : sc[19u + WF_BEAT_GROOVE];
        for (i = 0; i < nb; i++)
            if (sw[2u * i] == pat) {
                pat = sw[2u * i + 1u];
                break;
            }
        if (pat != WF_NONE) {
            const uint8_t *pr = wctx.b + wctx.pat[pat];
            p[P_SLEN] = pr[1];
            p[P_SDIV] = pr[0] & WF_PAT_DIVMASK;
        }
        if (t == kt) {                                    /* the keys track: its loop, and PULSE's arp group */
            pat = WF_NONE;
            if (wst.sw) {
                const uint8_t *pg = wctx.b + wctx.off[WF_S_PROGS];
                uint32_t beats = 0, k;
                for (k = 0; k < sc[12]; k++)              /* (records are variable: walk to this scene's) */
                    pg += 1u + 2u * pg[0];
                for (k = 0; k < pg[0]; k++)
                    beats += pg[2u + 2u * k];
                p[P_SLEN] = (int16_t)(16u * (beats >= 16u ? 4u : beats / 4u));   /* min(bars, 4) at 1/16 */
                p[P_SDIV] = WF_DIV_16;
                for (k = P_AMODE; k <= P_AORDER; k++)
                    p[k] = TP[k].def;
                for (k = 0; k < 4u; k++)
                    p[P_AMODE + k] = WB_PULSE[def[6]][k];
            } else {
                p[P_SLEN] = trk[t].p[P_SLEN];
                p[P_SDIV] = trk[t].p[P_SDIV];
                for (i = P_AMODE; i <= P_AORDER; i++)
                    p[i] = trk[t].p[i];
            }
        }
        if (!wst.sw)
            p[P_MUTE] = trk[t].p[P_MUTE];                 /* (the player's; a new World starts unmuted) */
        for (i = 0; i < P_COUNT; i++) {                   /* every value inside its range (as proj_apply) */
            const param_desc_t *d = t == TRK_DRUM ? (i == P_E0 ? &DRUM_KIT_DESC : i > P_E0 ? 0 : &TP[i])
                                                  : i >= P_E0 ? &ENGINES[e]->edit[i - P_E0] : &TP[i];
            if (d)
                p[i] = (int16_t)clamp(p[i], d->min, d->max);
        }
        wst.pat[t] = (uint8_t)pat;
        wst.eng[t] = (uint8_t)e;
        wst.preset[t] = (uint8_t)(t == TRK_DRUM ? 0u : pre);
    }
    for (i = 0; i < sizeof WB_GWHITE; i++)                /* the globals: defaults, the World, variation, scene */
        wst.g[WB_GWHITE[i]] = GP[WB_GWHITE[i]].def;
    wst.g[G_SWING] = meta[5];
    if (wctx.have >> WF_S_GLOBALS & 1u)
        for (i = 0; i < wctx.cnt[WF_S_GLOBALS]; i++)
            wst.g[wctx.b[wctx.off[WF_S_GLOBALS] + WF_PAIR * i]] = (int8_t)wctx.b[wctx.off[WF_S_GLOBALS] + WF_PAIR * i + 1u];
    ws_spairs(vp + 1, vp[0], WF_SCOPE_G, wst.g);
    ws_spairs(sc + WF_SCENE_HDR, sc[23], WF_SCOPE_G, wst.g);
    for (i = 0; i < sizeof WB_GWHITE; i++) {
        uint32_t g = WB_GWHITE[i];
        wst.g[g] = (int16_t)clamp(wst.g[g], GP[g].min, GP[g].max);
    }
    ws_keys(sc[12]);                                      /* the progression, the key maps (Phase 6) */
    {   /* the scene's ENERGY table, the variation's bias, GUARD's arrangement timing (arrange.c, Phase 7) */
        const uint8_t *e = 0, *pg = wctx.b + wctx.off[WF_S_PROGS];
        uint32_t k, beats = 0;
        if (sc[13] != WF_NONE)
            for (e = wctx.b + wctx.off[WF_S_ENERGY], k = 0; k < sc[13]; k++)
                e += WF_ENERGY_HDR + WF_BAND_LEN * e[2];
        for (k = 0; k < sc[12]; k++)
            pg += WF_PROG_HDR + 2u * pg[0];
        for (k = 0; k < pg[0]; k++)
            beats += pg[2u + 2u * k];
        arr_stage(&wst.et, e, wctx.have >> WF_S_GUARD & 1u ? wctx.b + wctx.off[WF_S_GUARD] : 0, beats, sc[15],
                  (int8_t)vr[-1]);
    }
    wst.st = WST_READY;
    return WE_OK;
}

/* ----------------------------------------------------------------- commit --- */
/* the READY stage into the instrument. The audio ISR (on a boundary, Phase 11) or the main loop with IRQs off.
 * Held keys and the keys loop stay (except at a World switch); an engine change fades (voice.c engine_block). */
static void world_commit(void)
{
    uint32_t t, i, ovin = ov_in;
    if (wst.st != WST_READY)
        return;
    if (ovin)
        ov_restore();                                     /* (inside a block: the bases back, then the new ones) */
    for (t = 0; t < NTRK; t++) {
        track_t *k = &trk[t];
        uint32_t np = wst.pat[t], cp = wrt.cur_pat[t];
        if (t != wrt.keys_trk || wst.sw) {
            if (!wst.sw && cp != np && cp < WF_MAX_PAT)
                memcpy(&wpool[cp], k->step, sizeof k->step);   /* the outgoing pattern, with any edits */
            if (cp != np || wst.sw) {
                if (np < WF_MAX_PAT)
                    memcpy(k->step, &wpool[np], sizeof k->step);
                else
                    steps_clear(k);
                wrt.cur_pat[t] = (uint8_t)np;
            }
        }
        memcpy(k->p, wst.p[t], sizeof k->p);
        if (t < NPART)
            k->eng_req = wst.eng[t];
        k->preset = wst.preset[t];
        k->user = 0;
    }
    for (i = 0; i < sizeof WB_GWHITE; i++)
        song.g[WB_GWHITE[i]] = wst.g[WB_GWHITE[i]];
    wrt.scene = wst.scene;
    wrt.var = wst.var;
    wrt.prog = wst.prog;
    wrt.energy = wst.energy;
    wrt.fill = wst.fill;
    harm_commit();                                        /* the progression (from chord 0) and the key maps */
    sk_commit(wst.sw);
    arr_commit(&wst.et);                                  /* its ENERGY table, the band at the position (Phase 7) */
    if (wst.sw) {                                         /* a World switch: a different instrument */
        song.g[G_BPM] = wst.bpm;
        panic_req = (uint8_t)((1u << NTRK) - 1u);
        song.sel = wrt.keys_trk;
        wst.sw = 0;
    }
    if (ovin)
        ov_apply(0);                                      /* the macros on the new bases (macro.c) */
    wst.st = WST_APPLIED;
}

/* stage and commit while stopped; the World becomes active (SLOOP's hooks step aside). Phase 11 adds the
 * commit on the bar while playing */
static int world_apply(uint32_t scene, uint32_t var)
{
    int rc;
    if (song.playing || transport_req)
        return WE_BUSY;
    rc = world_stage(scene, var);
    if (rc)
        return rc;
    fm1_irq_off();
    if (wst.sw || !wrt.active) {
        ov_reset();                                       /* another World: the old one's overlay goes (macro.c) */
        guard_reset();                                    /* .. and no CPU hold of the old one (guard.c) */
    }
    world_commit();
    if (!wrt.active) {                                    /* SLOOP -> a World: the pitch counts (H2) and Smart Keys */
        memset(vref, 0, sizeof vref);
        memset(vlive, 0, sizeof vlive);
        wrt.refcount = 1;
        wrt.keys_on = 1;                                  /* (PLAY. Phase 14's ADV_WORLD clears it: SLOOP's kb_map) */
    }
    wrt.active = 1;
    fm1_irq_on();
    wst.st = WST_FREE;
    macro_eval();                                         /* the macros' table now (a World: at its targets) */
    return WE_OK;
}

static int world_start(const uint8_t *b, uint32_t n)   /* load a World and play its default scene and variation */
{
    int rc = world_load(b, n);
    if (rc)
        return rc;
    return world_apply(wctx.b[wctx.off[WF_S_DEFAULTS]], wctx.b[wctx.off[WF_S_DEFAULTS] + 1u]);
}

static void world_unload(void)                         /* back to SLOOP's paths (ui_play.c restores the project) */
{
    fm1_irq_off();
    wrt.active = 0;
    wrt.refcount = 0;
    wrt.keys_on = 0;
    wrt.mute = 0;                                         /* (H1: no track left out by an ENERGY band) */
    wbeat_pat = WF_NONE;
    ov_reset();                                           /* (no macro overlay, no vmod offset: SLOOP's sound) */
    guard_reset();
    fm1_irq_on();
}

static int world_factory(uint32_t i, const uint8_t **b, uint32_t *n)   /* factory World i (sorted by category) */
{
    if (i >= WORLD_NFACTORY)
        return WE_STATE;
    *b = WORLD_DATA + WORLD_INDEX[i].off;
    *n = WORLD_INDEX[i].len;
    return WE_OK;
}

/* ------------------------------------------------------------------ hooks --- */
static void wreq_block(void);          /* (requests and accessors, at the end) */
static void world_block(void)          /* seq.c events_block (H15), audio ISR, while playing */
{
    if (!wrt.active)
        return;
    wreq_block();                      /* a READY stage, or a World switch, on the next bar */
    /* Phase 11: the scene's transition (2 and 4 bars), held-note continuity and tails */
}

static void world_boot(void)           /* main.c felucca_init after autosave_resume (H22) */
{
#if WORLD_NFACTORY
    uint32_t i;
    wb_ctx_t c;
    wrt.factory_ok = 0;
    for (i = 0; i < WORLD_NFACTORY && i < 32u; i++)       /* the factory Worlds in flash, as built */
        if (wb_check(WORLD_DATA + WORLD_INDEX[i].off, WORLD_INDEX[i].len, &c) == WE_OK && wb_u32(c.b + 8) == WORLD_INDEX[i].id)
            wrt.factory_ok |= 1u << i;
#endif
    /* Phase 5: SLOOP boots as it always did (wrt.active stays 0). Phase 9: the session decides, PLAY by default */
}

/* ======================================================= requests and accessors (Phase 6 part A) ======
 * For the UIs (the simulator, the Phase 9 PLAY screens) and the host tools: what the loaded World is called and
 * holds, read from the checked blob through wctx (never cast to a struct), and the requests that change it while
 * it plays. Main loop, except wreq_block (the audio ISR, through world_block).
 *
 *   world_request(scene, var)  stopped: world_apply now. Playing: world_stage, and the ISR commits the READY stage
 *                              on the next 4/4 bar, every track from its step 0 there (as SAVE + key's sections).
 *                              A newer request replaces one not yet committed
 *   world_switch(blob, n)      another World. Stopped (or from SLOOP): world_start now. Playing a World: on the
 *                              next bar the transport stops, world_service loads the new World and starts it
 *                              again: a restart on the bar, not a seamless change (Phase 11)
 *   world_service()            main loop, every pass: frees an APPLIED stage, finishes a switch
 *   world_pending(&s, &v)      a request waiting for its bar: 1 (s = WF_NONE: a World switch), else 0
 *   world_hot_reload(blob, n)  the simulator's authoring (design 2.8): the same World's new data, at once
 * The stage: the main loop writes it only when FREE (world_stage), the ISR commits READY -> APPLIED, the main loop
 * sets FREE again (world_service, world_request). Through a scene or variation change the keys track keeps its
 * loop and the held keys sound on (SLOOP's behaviour); the sequencer's own notes are released on the bar, so
 * nothing hangs. Phase 11 refines transitions: their 2 and 4 bars, held-note continuity, tails. */
static const char *const WF_ROLE_LABEL[WF_NROLES] = {"PAD", "CHORDS", "BASS", "LEAD", "KEYS", "TEXTURE", "DRUMS"};

static struct {
    uint32_t bar, beat;                /* clk_beat / 4 of the last bar the ISR saw; clk_beat then */
    const uint8_t *sw_b;               /* the World to switch to (it stays valid, as any loaded blob) */
    uint32_t sw_n;
    volatile uint8_t sw;               /* 1: switch on the next bar (main -> ISR); 2: stopped for it (ISR -> main) */
} wreq = {0xFFFFFFFFu, 0, 0, 0, 0};

/* ---- what the loaded World holds (the strings are NUL-terminated in the blob: wb_check made sure) */
static const char *world_meta_str(uint32_t at)
{
    return wrt.loaded ? (const char *)(wctx.b + wctx.off[WF_S_META] + at) : "";
}
static const char *world_name(void) { return world_meta_str(0); }
static const char *world_category(void) { return world_meta_str(WF_NAME_LEN); }
static const char *world_blurb(void) { return world_meta_str(WF_NAME_LEN + WF_CAT_LEN); }
static uint32_t world_bpm(void)        /* the authored tempo (the player's nudge is song.g[G_BPM]) */
{
    return wrt.loaded ? wctx.b[wctx.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN] : 0u;
}
static void world_tempo(uint32_t *lo, uint32_t *hi)     /* the authored tempo range (GLO + SELECT keeps inside) */
{
    const uint8_t *m = wctx.b + wctx.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN;
    *lo = wrt.loaded ? m[1] : (uint32_t)GP[G_BPM].min;
    *hi = wrt.loaded ? m[2] : (uint32_t)GP[G_BPM].max;
}
static uint32_t world_nscenes(void) { return WF_NSCENE; }
static const char *world_scene_name(uint32_t s)
{
    return wrt.loaded && s < WF_NSCENE ? (const char *)(wctx.b + wctx.scene[s]) : "";
}
static uint32_t world_nvar(void) { return wrt.loaded ? wctx.cnt[WF_S_VARS] : 0u; }
static const char *world_var_name(uint32_t v)
{
    return wrt.loaded && v < wctx.cnt[WF_S_VARS] ? (const char *)(wctx.b + wctx.var[v]) : "";
}
static uint32_t world_track_role(uint32_t t) { return wrt.loaded && t < NTRK ? wctx.b[wctx.trk[t]] : WF_NONE; }
static const char *world_role_label(uint32_t r) { return r < WF_NROLES ? WF_ROLE_LABEL[r] : "-"; }
static uint32_t world_keys_track(void) { return wrt.keys_trk; }
static uint32_t world_factory_count(void) { return WORLD_NFACTORY; }
static int world_factory_find(uint32_t id)   /* the factory index of a World id (sorted by category), or -1 */
{
    uint32_t i;
    for (i = 0; i < WORLD_NFACTORY; i++)
        if (WORLD_INDEX[i].id == id)
            return (int)i;
    return -1;
}
/* a factory World's name, category and tempo without loading it (its META, through wb_check) */
static int world_factory_info(uint32_t i, char *name, char *cat, uint32_t *bpm)
{
    const uint8_t *b, *m;
    uint32_t n;
    wb_ctx_t c;
    int rc = world_factory(i, &b, &n);
    if (rc || (rc = wb_check(b, n, &c)) != WE_OK)
        return rc;
    m = c.b + c.off[WF_S_META];
    str_cpy(name, (const char *)m, WF_NAME_LEN);
    str_cpy(cat, (const char *)m + WF_NAME_LEN, WF_CAT_LEN);
    *bpm = m[WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN];
    return WE_OK;
}

/* ---- requests */
static int world_request(uint32_t scene, uint32_t var)
{
    if (!wrt.loaded || scene >= WF_NSCENE || var >= wctx.cnt[WF_S_VARS])
        return WE_STATE;
    if (wreq.sw)
        return WE_BUSY;                /* (a World switch waits for its bar) */
    if (wst.st == WST_APPLIED)
        wst.st = WST_FREE;
    if (!song.playing && !transport_req)
        return world_apply(scene, var);
    if (!wrt.active)
        return WE_STATE;               /* (playing SLOOP: a World starts with world_switch) */
    return world_stage(scene, var);    /* READY: wreq_block commits it on the next bar */
}

static int world_pending(uint32_t *scene, uint32_t *var)
{
    if (wreq.sw) {
        *scene = *var = WF_NONE;
        return 1;
    }
    if (wst.st == WST_READY) {
        *scene = wst.scene;
        *var = wst.var;
        return 1;
    }
    return 0;
}

/* BEAT (design 9.2; SEQ in PLAY): the scene's drum pattern for beat b (WF_BEAT_*; none authored: its GROOVE) from
 * the next bar, without a clock reset (the drum track is bar-aligned): wreq_block swaps it in. Stopped: at once.
 * Later stages keep it (wrt.beat). Phase 11 adds the BEAT masks: MINIMAL's lanes, BUSY's density, BREAK's hats */
static void wbeat_swap(uint32_t pat)   /* the drum track to pool entry pat (ISR on a bar, or IRQs off) */
{
    track_t *d = TDRUM;
    uint32_t cp = wrt.cur_pat[TRK_DRUM];
    const uint8_t *pr;
    if (pat >= WF_MAX_PAT || pat == cp)
        return;
    pr = wctx.b + wctx.pat[pat];
    if (cp < WF_MAX_PAT)
        memcpy(&wpool[cp], d->dstep, sizeof d->dstep);   /* (with any edits) */
    memcpy(d->dstep, &wpool[pat], sizeof d->dstep);
    d->p[P_SLEN] = pr[1];
    d->p[P_SDIV] = pr[0] & WF_PAT_DIVMASK;
    wrt.cur_pat[TRK_DRUM] = (uint8_t)pat;
}
static int world_beat(uint32_t b)
{
    const uint8_t *sc;
    uint32_t pat;
    if (!wrt.active || b >= WF_NBEATS || wreq.sw)
        return WE_STATE;
    wrt.beat = (uint8_t)b;
    if (wst.st == WST_READY)           /* a scene on its way: staged again with this BEAT (it lands with the scene) */
        return world_stage(wst.scene, wst.var);
    sc = wctx.b + wctx.scene[wrt.scene];
    pat = sc[19u + b] != WF_NONE ? sc[19u + b] : sc[19u + WF_BEAT_GROOVE];
    fm1_irq_off();
    wbeat_pat = WF_NONE;
    if (!song.playing && !transport_req)
        wbeat_swap(pat);
    else if (pat != wrt.cur_pat[TRK_DRUM])
        wbeat_pat = (uint8_t)pat;
    fm1_irq_on();
    return WE_OK;
}

static int world_switch(const uint8_t *b, uint32_t n)
{
    wb_ctx_t c;
    int rc;
    if (wst.st == WST_APPLIED)
        wst.st = WST_FREE;
    if (!song.playing && !transport_req) {
        fm1_irq_off();
        if (wst.st == WST_READY)       /* (stopped: nothing waits for a bar) */
            wst.st = WST_FREE;
        fm1_irq_on();
        wreq.sw = 0;
        return world_start(b, n);
    }
    if ((rc = wb_check(b, n, &c)) != WE_OK)
        return rc;                     /* (refused now rather than on the bar) */
    fm1_irq_off();
    if (wst.st == WST_READY)           /* a scene request of the old World: dropped */
        wst.st = WST_FREE;
    wreq.sw_b = b;
    wreq.sw_n = n;
    if (wrt.active) {
        wreq.sw = 1;                   /* wreq_block stops the transport on the next bar */
    } else {
        wreq.sw = 2;                   /* playing SLOOP: stop now */
        transport_req = 2;
    }
    fm1_irq_on();
    return WE_OK;
}

static int world_service(void)         /* main loop: a switch's result (WE_*), or -1 when there was none */
{
    int rc = -1;
    if (wst.st == WST_APPLIED) {
        wst.st = WST_FREE;
        mac.dirty = 1;                 /* new bases: the guard's combinations and distorted tracks judged again */
    }
    if (wreq.sw == 2u && !song.playing && !transport_req) {
        wreq.sw = 0;
        rc = world_start(wreq.sw_b, wreq.sw_n);
        transport_req = 1;             /* on again: the new World (or the old one, when the new one failed) */
    }
    return rc;
}

static void wreq_block(void)           /* world_block: audio ISR, a World playing */
{
    uint32_t t;
    if (clk_beat < wreq.beat)
        wreq.bar = 0xFFFFFFFFu;        /* (the clock started again: its bar 0 is a new bar) */
    wreq.beat = clk_beat;
    if ((clk_beat & 3u) || (clk_beat >> 2) == wreq.bar)
        return;
    wreq.bar = clk_beat >> 2;          /* a new bar */
    if (wreq.sw == 1u) {
        wreq.sw = 2;                   /* stop here; world_service loads and starts the new World */
        transport_req = 2;
        return;
    }
    if (wst.st != WST_READY) {
        if (wbeat_pat != WF_NONE)      /* a BEAT: the drum pattern from this bar, the clock runs on */
            wbeat_swap(wbeat_pat);
        wbeat_pat = WF_NONE;
        return;
    }
    wbeat_pat = WF_NONE;               /* (the stage carries the BEAT)  */
    for (t = 0; t < NTRK; t++)
        seq_release(&trk[t]);          /* the sequencer's notes (not the held keys): nothing hangs */
    world_commit();
    seq_reset_tracks(clk_pos);         /* on the bar: every track from its step 0 */
    wreq.bar = wreq.beat = 0;
}

#define WORLD_STALE 254                /* (a cur_pat that is no pool entry: the commit copies the new steps) */
static int world_hot_reload(const uint8_t *b, uint32_t n)
{
    wb_ctx_t c;
    uint32_t i, t, var;
    int rc;
    if (!wrt.loaded || wreq.sw)
        return WE_STATE;
    if ((rc = wb_check(b, n, &c)) != WE_OK)
        return rc;                     /* (the old World plays on) */
    fm1_irq_off();
    wst.st = WST_FREE;                 /* (a request not yet committed: restaged below) */
    wctx = c;
    for (i = 0; i < c.cnt[WF_S_PATTERNS]; i++)
        wb_pattern(c.b + c.pat[i], &wpool[i]);
    if (c.keys != wrt.keys_trk)
        wst.sw = 1;                    /* (another keys track: as a new World) */
    wrt.keys_trk = c.keys;
    wrt.id = wb_u32(b + 8);
    for (t = 0; t < NTRK; t++)
        if (t != wrt.keys_trk || wst.sw)
            wrt.cur_pat[t] = WORLD_STALE;
    wb_macros(1);                                         /* (its mappings may have changed; the positions stay) */
    fm1_irq_on();
    var = wrt.var < c.cnt[WF_S_VARS] ? wrt.var : 0u;
    if ((rc = world_stage(wrt.scene, var)) != WE_OK)
        return rc;
    fm1_irq_off();
    for (t = 0; t < NTRK; t++)
        seq_release(&trk[t]);
    world_commit();
    song.g[G_BPM] = wst.bpm;           /* (the authored tempo, as the file now says) */
    fm1_irq_on();
    wst.st = WST_FREE;
    return WE_OK;
}
