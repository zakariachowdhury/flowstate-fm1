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
 *   world_commit the stage into trk[] / song.g (the ISR on its boundary, Phase 11; IRQs off while stopped)
 *   world_apply  stage + commit while stopped, and the World becomes active
 *
 * Main loop, except world_commit and world_block (audio ISR). It needs core.h .. seq.c and params.c's
 * preset_fill, nothing of the UI, so the host tests build it on tests/hostsim.c. It never reads or writes the
 * user's project slots (design D6; run_tests.sh greps for it): a World's scenes are its own. */
#include "felucca_worlds.h"            /* WORLD_DATA, WORLD_INDEX, WORLD_NFACTORY (tools/gen_worlds.py) */

typedef struct {                       /* a checked blob: where its sections and records are */
    const uint8_t *b;
    uint32_t len, have;                /* have: bit per section type present */
    uint16_t off[WF_S_OVERRIDES + 1], slen[WF_S_OVERRIDES + 1];   /* payload offset (from b) and length per type */
    uint8_t cnt[WF_S_OVERRIDES + 1];
    uint8_t scale, keys;               /* META scale, KEYS track */
    uint16_t trk[WF_NTRK], pat[WF_MAX_PAT], scene[WF_NSCENE], var[WF_MAX_VARS];   /* record offsets */
} wb_ctx_t;
static wb_ctx_t wctx;                  /* the loaded World */
static wb_ctx_t wnext;                 /* a World switch: the next World (checked), staged while the old one plays */
static uint8_t wbeat_pat, wbeat_m = WF_NONE;   /* BEAT: the drum pattern and masks (arrange.c arr_beat) wreq_block
                                                * takes on the next bar (wbeat_m WF_NONE: none) */
static uint8_t wnow;                   /* ADVANCED: a scene or variation commits at once (world_immediate) */
static uint8_t wvar_on = WF_NONE;      /* the variation whose macro defaults the controls took (wvar_macros) */

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
        if (var && p[0] == WF_SCOPE_CTL) {                /* a variation's macro default (Phase 12) */
            if (p[1] > 3u || p[2] > WF_UNIT)
                return 0;
        } else if (p[0] == WF_SCOPE_G) {
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
        WB_NEED(p[0] > prev && p[0] <= (b[5] & WF_F_USER ? WF_S_OVERRIDES : WF_S_LAST), WE_SECTION);
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
        WB_NEED(r[11] < WF_NSROLES && (r[14] == WF_TRANS_PHRASE || WB_HAS(WB_TRANS, r[14])), WE_SCENE);
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

    /* OVERRIDES (a user World, Phase 14): {scope, id, value}: a track's parameter (as in TRACKS; the drum track also
     * its kit), a whitelisted global, or a synth track's sound {128 | track, engine, preset} */
    if (c->have >> WF_S_OVERRIDES & 1u) {
        k = c->cnt[WF_S_OVERRIDES];
        p = b + c->off[WF_S_OVERRIDES];
        WB_NEED(k >= 1u && k <= WF_MAX_OVR + NPART, WE_COUNT);
        WB_NEED(c->slen[WF_S_OVERRIDES] == WF_SPAIR * k, WE_LENGTH);
        for (i = 0; i < k; i++, p += WF_SPAIR)
            WB_NEED(p[0] == WF_SCOPE_G ? WB_HAS(WB_GWHITE, p[1]) :
                    p[0] < NTRK ? wb_pid_ok(c, p[0], p[1], 0) || (p[0] == TRK_DRUM && p[1] == P_E0 && p[2] < DRUM_KITS) :
                    p[0] >= WF_OVR_SOUND && p[0] < WF_OVR_SOUND + NPART && p[1] < NENGINES &&
                    p[2] < ENGINES[p[1]]->npresets, WE_PARAM);
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

/* Phase 14: the working World (design 10.2). What the player changed in ADVANCED over the World as loaded: the
 * parameter and sound overrides (world_capture; the blob's OVERRIDES records, applied by world_stage after the scene),
 * the edited steps in the pool (written back at every commit) and each pool entry's length and division */
static struct {
    uint8_t n;
    uint8_t r[WF_MAX_OVR + NPART][WF_SPAIR];   /* {scope, id, value}, as a user World's OVERRIDES section */
} wovr;
static uint8_t wpl[WF_MAX_PAT][2];     /* each pool entry's len and div (the record's, until an edit) */
static uint8_t wcap;                   /* world_stage for world_capture: no parameter overrides, nothing READY */
static uint32_t wsaved;                /* world_sum as loaded or saved (world_dirty) */
static uint32_t world_sum(void)        /* the pool, its lengths and the overrides: what a save keeps */
{
    return wb_crc_run(wb_crc_run(wb_crc_run(0, (const uint8_t *)wpool, sizeof wpool), wpl[0], sizeof wpl), &wovr.n,
                      1u + WF_SPAIR * wovr.n);
}
static void wb_pool(const wb_ctx_t *c) /* a checked World's patterns into the pool, its overrides (none: a factory one) */
{
    uint32_t i;
    for (i = 0; i < c->cnt[WF_S_PATTERNS]; i++) {
        wb_pattern(c->b + c->pat[i], &wpool[i]);
        wpl[i][0] = c->b[c->pat[i] + 1u];
        wpl[i][1] = c->b[c->pat[i]] & WF_PAT_DIVMASK;
    }
    wovr.n = c->have >> WF_S_OVERRIDES & 1u ? c->cnt[WF_S_OVERRIDES] : 0u;
    memcpy(wovr.r, c->b + c->off[WF_S_OVERRIDES], WF_SPAIR * wovr.n);
    wsaved = world_sum();
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
 * Not while a World plays (world_switch then stages it through world_service) or a stage is pending. */
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
    wb_pool(&c);
    for (i = 0; i < NTRK; i++)
        wrt.cur_pat[i] = WF_NONE;                         /* (the pool is new: nothing to write back into it) */
    wbeat_m = wvar_on = WF_NONE;
    wrt.swp = 0;                                          /* (a World switch staged while playing: replaced) */
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
    uint32_t t, i, na, nb, kt = wst.sw ? wctx.keys : wrt.keys_trk;
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
    wst.eo = 0;
    wst.prog = sc[12];
    wst.energy = sc[13];
    wst.fill = sc[15];
    wst.bpm = meta[0];
    for (t = 0; t < NTRK; t++) {
        const uint8_t *tr = wctx.b + wctx.trk[t];
        int16_t *p = wst.p[t];
        uint32_t e = t == TRK_DRUM ? 0u : tr[1], pre = tr[2], pat, eo = 0;
        int16_t pe[8];
        for (i = 0; i < na; i++)                          /* the variation's sound (the drum track: its kit) */
            if (vr[1u + 2u * i] == t)
                pre = vr[2u + 2u * i];
        for (i = 0; i < wovr.n; i++)                      /* the player's sound (Phase 14): another preset or engine */
            if (wovr.r[i][0] == (WF_OVR_SOUND | t)) {
                e = wovr.r[i][1];
                pre = wovr.r[i][2];
                eo = e != tr[1];
            }
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
        memcpy(pe, p + P_E0, sizeof pe);
        for (i = 0; i < tr[4]; i++)                       /* the World's sound */
            p[tr[WF_TRACK_HDR + WF_PAIR * i]] = (int8_t)tr[WF_TRACK_HDR + WF_PAIR * i + 1u];
        ws_spairs(vp + 1, vp[0], t, p);                   /* the variation */
        ws_spairs(sc + WF_SCENE_HDR, sc[23], t, p);       /* the scene */
        if (eo)
            memcpy(p + P_E0, pe, sizeof pe);              /* (another engine: the World's engine values are not its) */
        if (!wcap)
            ws_spairs(wovr.r[0], wovr.n, t, p);           /* the player's edits (Phase 14): in every scene, variation */
        wst.eo |= (uint8_t)(eo << t);
        /* the pattern: the scene's (the drum track: the BEAT's, else the GROOVE), through the variation's swaps */
        pat = t < NPART ? sc[16u + t] : sc[19u + wrt.beat] != WF_NONE ? sc[19u + wrt.beat] : sc[19u + WF_BEAT_GROOVE];
        for (i = 0; i < nb; i++)
            if (sw[2u * i] == pat) {
                pat = sw[2u * i + 1u];
                break;
            }
        if (pat != WF_NONE) {
            p[P_SLEN] = wpl[pat][0];                      /* (the pattern's, as the player left it) */
            p[P_SDIV] = wpl[pat][1];
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
                if ((pat = wctx.b[wctx.off[WF_S_KEYS] + 7u]) != WF_NONE) {   /* a user World's loop (Phase 14) */
                    p[P_SLEN] = wpl[pat][0];
                    p[P_SDIV] = wpl[pat][1];
                }
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
    if (!wcap)
        ws_spairs(wovr.r[0], wovr.n, WF_SCOPE_G, wst.g);
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
    /* Phase 11: the BEAT's masks (none when the scene has its pattern), the keys track, the ENERGY position, and the
     * boundary: a variation (or a World switch) the next bar line, a scene its transition (1, 2, 4 bars or the phrase
     * playing) counted from the section start; ADVANCED with world_immediate: at once */
    wst.et.bm = (uint8_t)(wrt.beat | (sc[19u + wrt.beat] != WF_NONE) << 2);
    wst.kt = (uint8_t)kt;
    wst.ereq = arr.req;
    wst.qp = 0;
    i = 1;
    if (scene != wrt.scene && !wst.sw && (i = sc[14]) == WF_TRANS_PHRASE) {
        i = hprog[hcur].beats >= 8u ? hprog[hcur].beats / 4u : 1u;
        wst.qp = 1;
    }
    wst.q = (uint8_t)(wnow && wrt.mode == WM_ADV && !wst.sw ? 0u : i);
    wst.st = wcap ? WST_FREE : WST_READY;
    return WE_OK;
}

/* ----------------------------------------------------------------- commit --- */
/* the READY stage into the instrument. The audio ISR (on its boundary, gl = 1: the values that click glide) or the
 * main loop with IRQs off. Held keys and the keys loop stay (except at a World switch); an engine change fades (voice.c
 * engine_block). A World switch also leaves the old World's macro overlay, CPU hold and pending BEAT behind */
static const uint8_t WB_GLIDE[] = {P_LEVEL, P_PAN, P_DIST, P_CHOR, P_DLY, P_REV, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6,
                                   P_E7};          /* glide while playing: all, the EDIT values with the same engine */
#define WB_GLIDE_COMMON 6
static void world_commit(uint32_t gl)
{
    uint32_t t, i, ovin = ov_in;
    int16_t o[sizeof WB_GLIDE];
    if (wst.st != WST_READY)
        return;
    if (ovin)
        ov_restore();                                     /* (inside a block: the bases back, then the new ones) */
    wgl_n = 0;                                            /* (glides of a commit before: on from where they are) */
    wbeat_m = WF_NONE;                                    /* (the stage carries the BEAT) */
    if (wst.sw) {
        wrt.keys_trk = wst.kt;
        ov_reset();
        guard_reset();
        arr.req = wst.ereq;
    }
    for (t = 0; t < NTRK; t++) {
        track_t *k = &trk[t];
        uint32_t np = wst.pat[t], cp = wrt.cur_pat[t], e = k->engine % NENGINES;
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
        if (t == wrt.keys_trk && !wst.sw) {               /* the keys loop's length as it is now (PLAY REC closes */
            wst.p[t][P_SLEN] = k->p[P_SLEN];             /* a take in the ISR: the stage's copy may be older) */
            wst.p[t][P_SDIV] = k->p[P_SDIV];
        }
        for (i = 0; i < sizeof WB_GLIDE; i++)
            o[i] = k->p[WB_GLIDE[i]];
        memcpy(k->p, wst.p[t], sizeof k->p);
        for (i = 0; gl && i < sizeof WB_GLIDE; i++)
            if (i < WB_GLIDE_COMMON || (t < NPART && wst.eng[t] == e && k->eng_req == e &&
                                        ENGINES[e]->edit[i - WB_GLIDE_COMMON].fmt != F_ENUM))
                wgl_add(&k->p[WB_GLIDE[i]], o[i]);
        if (t < NPART)
            k->eng_req = wst.eng[t];
        k->preset = wst.preset[t];
        k->user = 0;
    }
    for (i = 0; i < sizeof WB_GWHITE; i++) {
        int16_t *g = &song.g[WB_GWHITE[i]], b = *g;
        *g = wst.g[WB_GWHITE[i]];
        if (gl && !WB_HAS(WB_GSTRUCT, WB_GWHITE[i]))
            wgl_add(g, b);                                /* (the buses: SWING and DTIME change on the bar, fx.c) */
    }
    wrt.scene = wst.scene;
    wrt.var = wst.var;
    wrt.eo = wst.eo;
    wrt.prog = wst.prog;
    wrt.energy = wst.energy;
    wrt.fill = wst.fill;
    harm_commit();                                        /* the progression (from chord 0) and the key maps */
    sk_commit(wst.sw);
    arr_commit(&wst.et);                                  /* its ENERGY table, the band at the position (Phase 7) */
    if (wst.sw) {                                         /* a World switch: a different instrument */
        prec_reset();                                     /* (its keys loop is cleared above: no take, no ring) */
        prec.layers = wst.pat[wst.kt] < WF_MAX_PAT;       /* (a user World's own loop, Phase 14: copied in above) */
        prec_sync();
        song.g[G_BPM] = wst.bpm;
        panic_req = (uint8_t)((1u << NTRK) - 1u);
        song.sel = wrt.keys_trk;
        wst.sw = 0;
    }
    if (ovin)
        ov_apply(0);                                      /* the macros on the new bases (macro.c) */
    wst.st = WST_APPLIED;
}

/* Phase 12: a variation committed (or a World loaded), the main loop: each macro (COLOR MOTION SPACE ENERGY) the player
 * has not turned since the World loaded (macro.c mac.touched) goes to this variation's default (its VARS "macros",
 * scope WF_SCOPE_CTL pairs), else to the World's DEFAULTS; one the player has turned stays. Playing, the overlay glides
 * there (macro_eval without snap: each slot at its smoothing class); ENERGY's band follows with its own timing */
static void wvar_macros(void)
{
    const uint8_t *def = wctx.b + wctx.off[WF_S_DEFAULTS], *vr, *q;
    uint32_t c, n, to[4], user = wvar_on == WF_NONE && (wctx.b[5] & WF_F_USER);
    if (wrt.var == wvar_on)
        return;
    wvar_on = wrt.var;
    for (c = 0; c < 4u; c++)
        to[c] = def[2u + c] * 4u;
    vr = wctx.b + wctx.var[wrt.var] + WF_LABEL_LEN + 1u;  /* nsound, sounds, nswap, swaps, npairs, pairs */
    q = vr + 2u + 2u * vr[0];
    q += 2u * q[-1];
    for (n = user ? 0u : *q++; n; n--, q += WF_SPAIR)  /* (a user World loads at the positions it was saved with) */
        if (q[0] == WF_SCOPE_CTL)
            to[q[1] & 3u] = q[2] * 4u;
    for (c = 0; c < 4u; c++)
        if (!(mac.touched >> c & 1u) && mac.pos[c] != to[c]) {
            mac.pos[c] = (uint16_t)to[c];
            mac.dirty = 1;
            if (c == MC_ENERGY)
                arr.req = (uint16_t)to[c];
        }
}

/* stage and commit while stopped; the World becomes active (SLOOP's hooks step aside). Playing: world_request stages,
 * wreq_block commits on the boundary */
static int world_apply(uint32_t scene, uint32_t var)
{
    int rc;
    if (song.playing || transport_req)
        return WE_BUSY;
    rc = world_stage(scene, var);
    if (rc)
        return rc;
    fm1_irq_off();
    world_commit(0);                                      /* (another World: the old one's overlay and CPU hold go) */
    if (!wrt.active) {                                    /* SLOOP -> a World: the pitch counts (H2) and Smart Keys */
        memset(vref, 0, sizeof vref);
        memset(vlive, 0, sizeof vlive);
        wrt.refcount = 1;
        wrt.keys_on = 1;                                  /* (PLAY. Phase 14's ADV_WORLD clears it: SLOOP's kb_map) */
    }
    wrt.active = 1;
    fm1_irq_on();
    wst.st = WST_FREE;
    wvar_macros();                                        /* (the variation's macro defaults, Phase 12) */
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
    wrt.swp = 0;
    wbeat_m = WF_NONE;
    ov_reset();                                           /* (no macro overlay, no vmod offset: SLOOP's sound) */
    guard_reset();
    prec_reset();                                         /* (no PLAY REC, no keys grid offset) */
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
    wreq_block();                      /* a READY stage (a scene, a variation, a World) on its boundary, a BEAT */
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
 *                              on its boundary (Phase 11): a scene on the bar line its transition names (every 1, 2
 *                              or 4 bars, or every phrase, counted from the section start), every track from its
 *                              step 0 there; a variation on the next bar line, the clock running on (phase-locked).
 *                              A newer request replaces one not yet committed. ADVANCED with world_immediate(1): at
 *                              the next block, the clock running on
 *   world_switch(blob, n)      another World. Stopped: world_start now. Playing a World (Phase 11, seamless):
 *                              world_service decodes its patterns into the pool and stages its default scene while
 *                              the old World plays on from trk[] (the pool is read only at commits, BEAT swaps and
 *                              fills, none of which happen meanwhile); the ISR commits it on the next bar without a
 *                              stop: the FX tails ring on, the old voices release (or fade on an engine change),
 *                              the keys loop is cleared, the overlay ramps in from neutral. Playing SLOOP: a stop,
 *                              then world_start and PLAY
 *   world_service()            main loop, every pass: frees an APPLIED stage (a World switch: the new World's
 *                              macros, GUARD and name), stages a World switch
 *   world_pending(&s, &v)      a request waiting for its boundary: 1 (s = WF_NONE: a World switch), else 0
 *   world_bars_left(&ph)       the bar lines until it lands (1: the next one), 0: none; ph: on the phrase
 *   world_hot_reload(blob, n)  the simulator's authoring (design 2.8): the same World's new data, at once
 * The stage: the main loop writes it only when FREE (world_stage), the ISR commits READY -> APPLIED, the main loop
 * sets FREE again (world_service, world_request). Through a scene or variation change the keys track keeps its
 * loop in phase and the held keys sound on; the sequencer's own notes are released on the boundary (a variation:
 * those of the tracks whose pattern changes), so nothing hangs and no step plays twice. While playing, the values
 * that click when they jump glide (world_commit, macro.c wgl_*), a delay time crossfades (fx.c). */
static const char *const WF_ROLE_LABEL[WF_NROLES] = {"PAD", "CHORDS", "BASS", "LEAD", "KEYS", "TEXTURE", "DRUMS"};

static struct {
    uint32_t bar, beat;                /* clk_beat / 4 of the last bar the ISR saw; clk_beat then */
    volatile uint8_t sw;               /* 1: a World switch (wnext) staged by world_service, committed on the next
                                        * bar; 2: from SLOOP, started once the transport has stopped */
} wreq = {0xFFFFFFFFu, 0, 0};

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
static void wsv_free(void)             /* an APPLIED stage FREE again; after a World switch, the new World's rest */
{
    if (wst.st != WST_APPLIED)
        return;
    if (wrt.swp) {                     /* (the ISR committed it: its name, macros and GUARD now, ramping from neutral) */
        wctx = wnext;
        wrt.id = wb_u32(wctx.b + 8);
        wb_macros(0);
        mac.snap = wrt.swp = wreq.sw = 0;
        wvar_on = WF_NONE;
    }
    wst.st = WST_FREE;
    mac.dirty = 1;                     /* new bases: the guard's combinations and distorted tracks judged again */
    wvar_macros();                     /* (a new variation: its macro defaults, gliding) */
}

static int world_request(uint32_t scene, uint32_t var)
{
    wsv_free();
    if (!wrt.loaded || scene >= WF_NSCENE || var >= wctx.cnt[WF_S_VARS])
        return WE_STATE;
    if (wreq.sw)
        return WE_BUSY;                /* (a World switch waits for its bar) */
    if (!song.playing && !transport_req)
        return world_apply(scene, var);
    if (!wrt.active)
        return WE_STATE;               /* (playing SLOOP: a World starts with world_switch) */
    return world_stage(scene, var);    /* READY: wreq_block commits it on its boundary */
}
static void world_immediate(int on) { wnow = (uint8_t)(on != 0); }   /* ADVANCED only (world_stage) */

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
static uint32_t world_bars_left(int *ph)
{
    uint32_t q = wst.q;
    *ph = wst.qp;
    if (wreq.sw)
        return 1;
    return wst.st == WST_READY && q ? q - (clk_beat >> 2) % q : 0u;
}

/* BEAT (design 9.2; SEQ in PLAY): the scene's drum pattern for beat b (WF_BEAT_*; none authored: its GROOVE through
 * the BEAT's mask, arrange.c) from the next bar, without a clock reset (the drum track is bar-aligned): wreq_block
 * swaps it in. Stopped: at once. Later stages keep it (wrt.beat) */
static void wbeat_swap(uint32_t pat)   /* the drum track to pool entry pat (ISR on a bar, or IRQs off) */
{
    track_t *d = TDRUM;
    uint32_t cp = wrt.cur_pat[TRK_DRUM];
    if (pat >= WF_MAX_PAT || pat == cp)
        return;
    if (cp < WF_MAX_PAT)
        memcpy(&wpool[cp], d->dstep, sizeof d->dstep);   /* (with any edits) */
    memcpy(d->dstep, &wpool[pat], sizeof d->dstep);
    d->p[P_SLEN] = wpl[pat][0];
    d->p[P_SDIV] = wpl[pat][1];
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
    wbeat_pat = (uint8_t)pat;
    wbeat_m = (uint8_t)(b | (sc[19u + b] != WF_NONE) << 2);
    if (!song.playing && !transport_req) {
        wbeat_swap(pat);
        arr_beat(wbeat_m);
        wbeat_m = WF_NONE;
    }
    fm1_irq_on();
    return WE_OK;
}

static int world_switch(const uint8_t *b, uint32_t n)
{
    int rc;
    wsv_free();
    if (!song.playing && !transport_req) {
        fm1_irq_off();
        if (wst.st == WST_READY)       /* (stopped: nothing waits for a bar) */
            wst.st = WST_FREE;
        fm1_irq_on();
        wreq.sw = 0;
        return world_start(b, n);
    }
    if ((rc = wb_check(b, n, &wnext)) != WE_OK)
        return rc;                     /* (refused now rather than on the bar) */
    fm1_irq_off();
    if (wst.st == WST_READY)           /* a request not yet committed (a scene, another World): dropped */
        wst.st = WST_FREE;
    wbeat_m = WF_NONE;
    arr.fill_now = 0;                  /* (the pool is the next World's from now: no fill reads it) */
    arr.et.fill = WF_NONE;
    if (wrt.active) {
        wreq.sw = 1;                   /* world_service stages it, wreq_block commits it on the next bar */
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
    wsv_free();
    if (wreq.sw == 2u && !song.playing && !transport_req) {
        wreq.sw = 0;
        rc = world_start(wnext.b, wnext.len);
        transport_req = 1;             /* on again: the new World (or the old one, when the new one failed) */
    }
    if (wreq.sw == 1u && wst.st == WST_FREE) {   /* a World switch while one plays: staged now, on the next bar */
        wb_ctx_t cur = wctx;
        const uint8_t *def;
        wrt.swp = 1;                   /* (macro_service waits: the old World's table stays until the commit) */
        wctx = wnext;
        wb_pool(&wctx);                /* (the old World's edits go: it leaves on the bar) */
        def = wctx.b + wctx.off[WF_S_DEFAULTS];
        wrt.beat = def[7];
        wst.sw = 1;
        world_stage(def[0], def[1]);   /* (its checked defaults: it cannot fail) */
        wst.ereq = (uint16_t)(def[2u + MC_ENERGY] * 4u);
        wctx = cur;                    /* (the old World's names until the commit) */
    }
    return rc;
}

/* world_block: audio ISR, a World playing. The bar lines and BEATs; a READY stage commits on the first block of its
 * boundary's bar (before any step of it: no old step plays there, the new step 0 once); immediate (q 0) at once */
static void wreq_block(void)
{
    uint32_t t, bar, reset;
    if (clk_beat < wreq.beat)
        wreq.bar = 0xFFFFFFFFu;        /* (the clock started again: its bar 0 is a new bar) */
    wreq.beat = clk_beat;
    bar = clk_beat >> 2;
    if (wst.st != WST_READY || wst.q) {
        if ((clk_beat & 3u) || bar == wreq.bar)
            return;
        wreq.bar = bar;                /* a new bar */
        if (wst.st != WST_READY || bar % wst.q) {
            if (wbeat_m != WF_NONE) {  /* a BEAT: its pattern and masks from this bar, the clock runs on */
                wbeat_swap(wbeat_pat);
                arr_beat(wbeat_m);
                wbeat_m = WF_NONE;
            }
            return;
        }
    }
    reset = wst.q && (wst.sw || wst.scene != wrt.scene);   /* a scene or a World on its bar: from step 0 */
    for (t = 0; t < NTRK; t++)         /* the sequencer's notes (not the held keys): nothing hangs. A variation: the */
        if (reset || wst.pat[t] != wrt.cur_pat[t] || (wst.scene != wrt.scene && t != wrt.keys_trk))   /* tracks */
            seq_release(&trk[t]);      /* whose pattern changes; at once: not the keys loop, which runs on */
    if (reset)
        prec_rebase();                 /* (the keys loop, a take: on from where they are, play_rec.c) */
    world_commit(1);
    if (reset) {
        seq_reset_tracks(clk_pos);     /* on the bar: every track from its step 0, the keys loop in its phase */
        wreq.bar = wreq.beat = 0;
    }
}

#define WORLD_STALE 254                /* (a cur_pat that is no pool entry: the commit copies the new steps) */
static int world_hot_reload(const uint8_t *b, uint32_t n)
{
    wb_ctx_t c;
    uint32_t t, var;
    int rc;
    if (!wrt.loaded || wreq.sw)
        return WE_STATE;
    if ((rc = wb_check(b, n, &c)) != WE_OK)
        return rc;                     /* (the old World plays on) */
    fm1_irq_off();
    wst.st = WST_FREE;                 /* (a request not yet committed: restaged below) */
    wctx = c;
    wb_pool(&c);                       /* (the file is the truth: the edits go) */
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
    world_commit(0);
    song.g[G_BPM] = wst.bpm;           /* (the authored tempo, as the file now says) */
    fm1_irq_on();
    wst.st = WST_FREE;
    return WE_OK;
}

/* ======================================================= the working World and user Worlds (Phase 14) ======
 * design 10.2, 10.3. Main loop.
 *
 *   world_capture  ADVANCED -> the working World (on leaving it, before a scene key, before a save): the glides at
 *                  their ends, the steps and lengths playing into their pool entries, then the overrides: the stage of
 *                  the scene and variation playing without the parameter overrides (wcap) is the reference; a synth
 *                  track's engine or preset the player changed becomes a sound record, each parameter he changed one
 *                  record with its value (an edit set back to the reference drops it; one untouched keeps its old
 *                  record, so an edit made in another scene stays). World pairs on the engine parameters of a track
 *                  playing another engine are not applied (that engine has other ones). Not captured: P_ROOT P_SCALE
 *                  P_SLEN P_SDIV P_MUTE (the World's key, the patterns, the player's mutes), the keys track's arp
 *                  group (PULSE), the drum track's parameters outside WF_P_DRUM except its kit, globals outside the
 *                  whitelist (the tempo is the session's). 1: some did not fit (WF_MAX_OVR)
 *   world_dirty    edits or a loop that the World as loaded or saved does not hold (LEAVE WORLD asks first)
 *   world_encode   the working World as a self-contained user World blob (flag USER): every section of the loaded
 *                  one as it is, except META (the name, MY WORLDS, the tempo), PATTERNS (the pool re-encoded, the keys
 *                  loop as one more pattern), KEYS (that pattern), DEFAULTS (scene, variation, controls, PULSE, BEAT
 *                  now) and OVERRIDES. It must pass wb_check. Its length, or 0: over cap
 *   world_adopt    the working World is that blob now (after a save): same pool, its name, id and sections */
static int wov_at(const uint8_t (*r)[WF_SPAIR], uint32_t n, uint32_t scope, uint32_t id)
{
    while (n--)
        if (r[n][0] == scope && ((scope & WF_OVR_SOUND) || r[n][1] == id))
            return (int)n;
    return -1;
}
static int wov_put(uint32_t scope, uint32_t id, int32_t v)
{
    if (wovr.n >= WF_MAX_OVR + NPART)
        return 1;
    wovr.r[wovr.n][0] = (uint8_t)scope;
    wovr.r[wovr.n][1] = (uint8_t)id;
    wovr.r[wovr.n++][2] = (uint8_t)v;                     /* (every value a pair may hold fits a byte: FWD1's rule) */
    return 0;
}
static int world_capture(void)
{
    uint8_t old[WF_MAX_OVR + NPART][WF_SPAIR], chg = 0;
    uint32_t on = wovr.n, t, i, ps = WF_NONE, pv = 0;
    int drop = 0;
    if (!wrt.active || wreq.sw)
        return 0;
    wsv_free();
    fm1_irq_off();
    if (wst.st == WST_READY) {                            /* (a change on its way: asked for again below) */
        ps = wst.scene;
        pv = wst.var;
        wst.st = WST_FREE;
    }
    for (i = 0; i < wgl_n; i++)
        *wgl[i].p = wgl[i].to;
    wgl_n = 0;
    for (t = 0; t < NTRK; t++) {
        uint32_t c = wrt.cur_pat[t], len = (uint32_t)trk[t].p[P_SLEN], dv = (uint32_t)trk[t].p[P_SDIV];
        if (t == wrt.keys_trk || c >= WF_MAX_PAT)
            continue;
        memcpy(&wpool[c], trk[t].step, sizeof wpool[c]);
        if (len >= 1u && len <= NSTEP && dv < WF_NDIV &&
            (t != TRK_DRUM || (dv == WF_DIV_16 && (len == 16u || len == 32u || len == 64u)))) {
            wpl[c][0] = (uint8_t)len;
            wpl[c][1] = (uint8_t)dv;
        }
    }
    fm1_irq_on();
    if (wst.st != WST_FREE)
        return 0;
    memcpy(old, wovr.r, sizeof old);
    wcap = 1;
    world_stage(wrt.scene, wrt.var);                      /* the sounds as staged, without the parameter edits */
    wovr.n = 0;
    for (t = 0; t < NPART; t++) {
        const track_t *k = &trk[t];
        uint32_t e = k->eng_req % NENGINES, pre = k->preset;
        int s = wov_at(old, on, WF_OVR_SOUND | t, 0);
        if (e != wst.eng[t] || (!k->user && pre != wst.preset[t])) {
            if (k->user || pre >= ENGINES[e]->npresets)
                pre = e == wst.eng[t] ? wst.preset[t] : 0u;   /* (a user preset: its values are the edits) */
            chg |= (uint8_t)(1u << t);
            wov_put(WF_OVR_SOUND | t, e, (int32_t)pre);
        } else if (s >= 0) {
            wov_put(old[s][0], old[s][1], old[s][2]);
        }
    }
    if (chg)
        world_stage(wrt.scene, wrt.var);                  /* (the new sounds: the reference for their parameters) */
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < P_COUNT; i++) {
            int32_t v = trk[t].p[i], b = wst.p[t][i];
            int s;
            if (WB_HAS(WB_PFIXED, i) || (t == TRK_DRUM ? !WB_HAS(WB_PDRUM, i) && i != P_E0 :
                                         t == wrt.keys_trk && i >= P_AMODE && i <= P_AORDER))
                continue;
            s = chg >> t & 1u ? -1 : wov_at(old, on, t, i);
            if (s >= 0 ? v == (int8_t)old[s][2] || v != b : v != b)
                drop |= wov_put(t, i, v);
        }
    for (i = 0; i < sizeof WB_GWHITE; i++) {
        uint32_t g = WB_GWHITE[i];
        int32_t v = song.g[g];
        int s = wov_at(old, on, WF_SCOPE_G, g);
        if (s >= 0 ? v == (int8_t)old[s][2] || v != wst.g[g] : v != wst.g[g])
            drop |= wov_put(WF_SCOPE_G, g, v);
    }
    wcap = 0;
    if (ps != WF_NONE)
        world_stage(ps, pv);
    return drop;
}

static int world_dirty(void)
{
    const track_t *k = &trk[wrt.keys_trk % NPART];
    uint32_t l = wctx.b[wctx.off[WF_S_KEYS] + 7u], n = (uint32_t)k->p[P_SLEN];
    if (!wrt.loaded)
        return 0;
    if (world_sum() != wsaved)
        return 1;
    return prec_has(k) && (l >= WF_MAX_PAT || n != wpl[l][0] || n > NSTEP || memcmp(k->step, &wpool[l], n * sizeof(step_t)));
}

static uint32_t we_kind(const step_t *s)
{
    return s->time == ST_NOTE && s->n ? WF_R_NOTE : s->time == ST_TIE ? WF_R_TIE : WF_R_REST;
}
static uint32_t we_synth(uint8_t *d, const step_t *s, uint32_t len)   /* steps -> a record stream (fwd1-format 6.1) */
{
    uint32_t o = 0, c = 0, a, k, kind, n, nf, m;
    while (c < len) {
        const step_t *x;
        for (a = 0; c + a < len && a < 63u && we_kind(&s[c + a]) == WF_R_REST; a++)
            ;
        if (c + a == len)
            break;                                        /* (the rest is REST: no END needed) */
        if (we_kind(&s[c + a]) != WF_R_NOTE) {            /* rests (63, or before a TIE), or TIEs */
            kind = a ? WF_R_REST : WF_R_TIE;
            if (!a)
                while (c + a < len && a < 63u && we_kind(&s[c + a]) == WF_R_TIE)
                    a++;
            d[o++] = (uint8_t)(kind << 6 | a);
            c += a;
            continue;
        }
        x = &s[c + a];
        c += a + 1u;
        n = x->n > 4u ? 4u : x->n;
        m = x->flags >> 2 & 7u;
        nf = n | (x->flags & SF_ACCENT ? WF_NF_ACCENT : 0u) | (x->flags & SF_SLIDE ? WF_NF_SLIDE : 0u) |
             (x->lvl || x->rat ? WF_NF_LVLRAT : 0u) | (x->vel ? WF_NF_VEL : 0u) | (m ? WF_NF_MICRO : 0u);
        d[o++] = (uint8_t)(WF_R_NOTE << 6 | a);
        d[o++] = (uint8_t)nf;
        for (k = 0; k < n; k++)
            d[o++] = x->note[k] & 127u;
        if (nf & WF_NF_LVLRAT) {
            d[o++] = x->lvl;
            d[o++] = x->rat;
        }
        if (x->vel)
            d[o++] = x->vel > 127u ? 127u : x->vel;
        if (m)
            d[o++] = (uint8_t)m;
    }
    return o;
}
static uint32_t we_drum(uint8_t *d, const dstep_t *s, uint32_t len)  /* steps -> lanes (fwd1-format 6.2) */
{
    uint32_t o = 1, l, st, q, h, nh, f, nl = 0, bytes = len / 8u;
    for (l = 0; l < DRUM_LANES; l++) {
        uint32_t sh = 2u * (l & 3u);
        memset(d + o + 1u, 0, bytes);
        for (st = nh = f = 0; st < len; st++)
            if (s[st].on[l >> 3] >> (l & 7u) & 1u) {
                d[o + 1u + (st >> 3)] |= (uint8_t)(1u << (st & 7u));
                nh++;
                f |= (s[st].lvl[l >> 2] >> sh & 3u ? WF_DL_LVL : 0u) | (s[st].rat[l >> 2] >> sh & 3u ? WF_DL_RAT : 0u);
            }
        if (!nh)
            continue;
        d[o] = (uint8_t)(l | f);
        o += 1u + bytes;
        for (q = 0; q < 2u; q++) {                        /* the levels, then the ratchets: 2 bits a hit */
            if (!(f & (q ? WF_DL_RAT : WF_DL_LVL)))
                continue;
            memset(d + o, 0, (2u * nh + 7u) / 8u);
            for (st = h = 0; st < len; st++)
                if (s[st].on[l >> 3] >> (l & 7u) & 1u) {
                    d[o + (h >> 2)] |= (uint8_t)(((q ? s[st].rat : s[st].lvl)[l >> 2] >> sh & 3u) << (2u * (h & 3u)));
                    h++;
                }
            o += (2u * nh + 7u) / 8u;
        }
        nl++;
    }
    d[0] = (uint8_t)nl;
    return o;
}
static uint32_t world_encode(uint8_t *o, uint32_t cap, const char *name, uint32_t pulse)
{
    const track_t *k = &trk[wrt.keys_trk % NPART];
    const uint8_t *src = wctx.b;
    uint8_t tmp[WF_PAT_HDR + 1u + DRUM_LANES * (1u + NSTEP / 8u + NSTEP / 2u)];
    uint32_t have, ns = 0, off, t, i, n, id = 2166136261u, np = wctx.cnt[WF_S_PATTERNS];
    uint32_t lp = src[wctx.off[WF_S_KEYS] + 7u], loop = prec_has(k) && k->p[P_SLEN] >= 1 && k->p[P_SLEN] <= NSTEP;
    if (!wrt.loaded || cap < WF_MIN_LEN)
        return 0;
    if (loop && lp == WF_NONE) {
        if (np >= WF_MAX_PAT)
            return 0;
        lp = np++;
    }
    if (lp < WF_MAX_PAT) {                                /* the keys loop: one more synth pattern (or none now) */
        memcpy(&wpool[lp], k->step, sizeof wpool[lp]);
        if (!loop)
            memset(&wpool[lp], 0, sizeof wpool[lp]);
        wpl[lp][0] = (uint8_t)(loop ? k->p[P_SLEN] : 16);
        wpl[lp][1] = (uint8_t)(loop ? k->p[P_SDIV] % WF_NDIV : WF_DIV_16);
    }
    have = (wctx.have & ~(1u << WF_S_OVERRIDES)) | (np ? 1u << WF_S_PATTERNS : 0u) | (wovr.n ? 1u << WF_S_OVERRIDES : 0u);
    for (t = 1; t <= WF_S_OVERRIDES; t++)
        ns += have >> t & 1u;
    off = WF_HDR + WF_SECENT * ns;
    memset(o, 0, WF_HDR);
    for (ns = 0, t = 1; t <= WF_S_OVERRIDES; t++) {
        uint8_t *e = o + WF_HDR + WF_SECENT * ns, *d = o + off;
        uint32_t cnt = t == WF_S_PATTERNS ? np : t == WF_S_OVERRIDES ? wovr.n : wctx.cnt[t];
        if (!(have >> t & 1u))
            continue;
        if (t == WF_S_PATTERNS) {
            for (n = 0, i = 0; i < np; i++) {
                uint32_t drum = i < wctx.cnt[t] && (src[wctx.pat[i]] & WF_PAT_DRUM), len = wpl[i][0], nb;
                nb = drum ? we_drum(tmp + WF_PAT_HDR, wpool[i].dstep, len) : we_synth(tmp + WF_PAT_HDR, wpool[i].step, len);
                tmp[0] = (uint8_t)((drum ? WF_PAT_DRUM : 0u) | wpl[i][1]);
                tmp[1] = (uint8_t)len;
                tmp[2] = (uint8_t)nb;
                tmp[3] = (uint8_t)(nb >> 8);
                if (off + n + WF_PAT_HDR + nb > cap)
                    return 0;
                memcpy(d + n, tmp, WF_PAT_HDR + nb);
                n += WF_PAT_HDR + nb;
            }
        } else {
            n = t == WF_S_OVERRIDES ? WF_SPAIR * wovr.n : wctx.slen[t];
            if (off + n > cap)
                return 0;
            memcpy(d, t == WF_S_OVERRIDES ? wovr.r[0] : src + wctx.off[t], n);
        }
        if (t == WF_S_META) {                             /* the name, MY WORLDS, the tempo playing */
            const uint8_t *m = src + wctx.off[t] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN;
            memset(d, 0, WF_NAME_LEN + WF_CAT_LEN);
            str_cpy((char *)d, name, WF_NAME_LEN);
            str_cpy((char *)d + WF_NAME_LEN, "MY WORLDS", WF_CAT_LEN);
            d[WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN] = (uint8_t)clamp(song.g[G_BPM], m[1], m[2]);
        } else if (t == WF_S_KEYS) {
            d[7] = (uint8_t)(loop ? lp : WF_NONE);
        } else if (t == WF_S_DEFAULTS) {                  /* scene var ctl[4] pulse beat shape[4] move[4] */
            d[0] = wrt.scene;
            d[1] = wrt.var;
            for (i = 0; i < 12u; i++)
                d[i < 4u ? 2u + i : 4u + i] = (uint8_t)(macro_pos(i) / 4u);
            if (pulse < WF_NPULSE)
                d[6] = (uint8_t)pulse;
            d[7] = wrt.beat;
        }
        e[0] = (uint8_t)t;
        e[1] = (uint8_t)cnt;
        e[2] = (uint8_t)n;
        e[3] = (uint8_t)(n >> 8);
        off += n;
        ns++;
    }
    memcpy(o, "FWD1", 4);
    o[4] = WF_VERSION;
    o[5] = WF_F_USER;
    o[6] = (uint8_t)off;
    o[7] = (uint8_t)(off >> 8);
    for (i = 0; name[i]; i++)
        id = (id ^ (uint8_t)name[i]) * 16777619u;         /* (FNV-1a of the name: user names are unique) */
    for (i = 0; i < 4u; i++)
        o[8u + i] = (uint8_t)(id >> 8u * i);
    o[16] = (uint8_t)ns;
    id = wb_crc(o, off);
    for (i = 0; i < 4u; i++)
        o[WF_CRC_AT + i] = (uint8_t)(id >> 8u * i);
    {
        wb_ctx_t c;
        return wb_check(o, off, &c) == WE_OK ? off : 0u;
    }
}
static int world_adopt(const uint8_t *b, uint32_t n)
{
    wb_ctx_t c;
    if (wb_check(b, n, &c) != WE_OK)
        return WE_STATE;
    wctx = c;
    wrt.id = wb_u32(b + 8);
    wb_macros(1);                                         /* (MAPS and GUARD read from the blob: this one now) */
    wsaved = world_sum();
    return WE_OK;
}
