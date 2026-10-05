/* SPDX-License-Identifier: GPL-3.0-only */
/* The factory Worlds a per-World test plays (Phase 18): tests/run_tests.sh puts tests/world_sample.py's choice in
 * FACTORY_WORLDS, their source ids ("neon_rain midnight_drive ..."); unset or empty, every factory World. A test of
 * the firmware's built-in Worlds (WORLD_INDEX, in the firmware's order) asks world_picked(WORLD_INDEX[i].id); one that
 * plays them in the chosen order asks world_picks(). Include after the World format (world_fmt.h, felucca_worlds.h) */
#include <stdlib.h>
#include <string.h>

static uint32_t world_pick_fnv(const char *s, size_t n)   /* the device id: FNV-1a over the source id (worldc) */
{
    uint32_t h = WF_FNV_BASIS;
    while (n--)
        h = (h ^ (uint8_t)*s++) * WF_FNV_PRIME;
    return h;
}
/* the next id of FACTORY_WORLDS from *s: its length (0: the end), *at its start */
static size_t world_pick_next(const char **s, const char **at)
{
    size_t n = 0;
    while (**s == ' ' || **s == ',' || **s == '\n' || **s == '\t')
        (*s)++;
    *at = *s;
    while ((*s)[n] && (*s)[n] != ' ' && (*s)[n] != ',' && (*s)[n] != '\n' && (*s)[n] != '\t')
        n++;
    *s += n;
    return n;
}
static int world_picked(uint32_t id)             /* a built-in World's id: in the choice? */
{
    const char *s = getenv("FACTORY_WORLDS"), *at;
    size_t n;
    if (!s || !*s)
        return 1;
    while ((n = world_pick_next(&s, &at)))
        if (world_pick_fnv(at, n) == id)
            return 1;
    return 0;
}
/* the chosen Worlds in their order: ix[] their factory indexes, name[] their ids (for file names; with no
 * FACTORY_WORLDS, every factory World, named after its display name: "OFF-WORLD" -> "off_world") */
static uint32_t world_picks(int *ix, char (*name)[32], uint32_t max)
{
    const char *s = getenv("FACTORY_WORLDS"), *at;
    uint32_t k = 0, i;
    size_t n;
    if (!s || !*s) {
        for (i = 0; i < WORLD_NFACTORY && k < max; i++) {
            char nm[32], cat[32], *o = name[k];
            uint32_t bpm, j;
            if (world_factory_info(i, nm, cat, &bpm))
                continue;
            for (j = 0; nm[j] && o - name[k] < 31; j++)
                if (nm[j] == ' ' || nm[j] == '-')
                    *o++ = '_';
                else if ((nm[j] >= 'A' && nm[j] <= 'Z') || (nm[j] >= '0' && nm[j] <= '9'))
                    *o++ = (char)(nm[j] >= 'A' && nm[j] <= 'Z' ? nm[j] - 'A' + 'a' : nm[j]);
            *o = 0;
            ix[k++] = (int)i;
        }
        return k;
    }
    while (k < max && (n = world_pick_next(&s, &at))) {
        uint32_t id = world_pick_fnv(at, n);
        for (i = 0; i < WORLD_NFACTORY && WORLD_INDEX[i].id != id; i++)
            ;
        if (i == WORLD_NFACTORY) {
            printf("FACTORY_WORLDS: %.*s is not a built-in factory World (build/gen out of date?)\n", (int)n, at);
            exit(2);
        }
        ix[k] = (int)i;
        snprintf(name[k], 32, "%.*s", (int)(n < 31 ? n : 31), at);
        k++;
    }
    return k;
}
