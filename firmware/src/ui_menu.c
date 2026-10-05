/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SLOOP menu (HOME held): COLOR, LOWCUT, ZOOM, HARDWARE CALIBRATION, ABOUT. With Musical Worlds (H24) first PLAY
 * MODE (from SLOOP and ADVANCED) and LEAVE WORLD (in a World session). */
#ifndef FELUCCA_BUILD_DATE
#define FELUCCA_BUILD_DATE __DATE__  /* build.py: SOURCE_DATE_EPOCH pins it (reproducible builds) */
#endif
/* ------------------------------------------------------------ menu --- */
enum { MI_COLOR, MI_LOWCUT, MI_ZOOM, MI_PANEL, MI_ABOUT, MI_BACK, MI_PLAY, MI_LEAVE, MI_COUNT };
static const char *const MI_NAME[MI_COUNT] = {"COLOR", "LOWCUT", "ZOOM", "HARDWARE CALIBRATION", "ABOUT", "BACK",
                                              "PLAY MODE", "LEAVE WORLD"};

static uint32_t menu_items(uint8_t *ids)               /* the rows shown, in order; returns how many */
{
    uint32_t n = 0, i;
#if FELUCCA_WORLD
    if (wrt.mode != WM_PLAY)
        ids[n++] = MI_PLAY;
    if (wrt.mode != WM_SLOOP)
        ids[n++] = MI_LEAVE;
#endif
    for (i = 0; i <= MI_BACK; i++)
        ids[n++] = (uint8_t)i;
    return n;
}

static void draw_menu(void)
{
    uint8_t ids[MI_COUNT];
    uint32_t i, pass, nmi = menu_items(ids), sig = ui.menu * 7u + ui.menu_sel * 131u + settings.palette * 1009u +
                                                  settings.lowcut * 7919u + settings.zoom * 104729u + nmi * 3u;
    if (!ui.force && sig == ui.menu_sig)
        return;
    ui.menu_sig = sig;
    if (ui.force)                                   /* head + rule + two bands cover rows 0..229 */
        lcd_fill(0, H_HEAD + 1 + 124 + 95, 240, 240 - (H_HEAD + 1 + 124 + 95), C_BLACK);
    cv_begin(240, H_HEAD, C_BLACK);
    cv_text(4, 1, &FONT_S, ui.menu == 2 ? "ABOUT" : "MENU", C_HI);
    cv_blit(0, Y_HEAD);
    lcd_fill(0, H_HEAD, 240, 1, C_LINE);
    for (pass = 0; pass < 2u; pass++) {             /* the canvas holds 124 rows: draw in two bands */
        cv_begin(240, pass ? 95u : 124u, C_BLACK);
        cv_oy = pass ? -124 : 0;
        if (ui.menu == 2) {
            cv_text(4, 4, &FONT_L, "SLOOP", C_WHITE);
            cv_rect(96, 10, 8, 4, TE_COL[0]), cv_rect(96, 16, 12, 4, TE_COL[1]);   /* the sail */
            cv_rect(96, 22, 16, 4, TE_COL[2]), cv_rect(96, 28, 20, 4, TE_COL[3]);
            cv_text(4, 36, &FONT_S, "BASED ON FELUCCA", C_AMB);
            cv_text(4, 54, &FONT_S, FELUCCA_VERSION, C_HI);
            cv_text(236 - text_w(&FONT_S, FELUCCA_BUILD_DATE), 54, &FONT_S, FELUCCA_BUILD_DATE, C_GRAY); /* build date */
            cv_text(cv_text(4, 72, &FONT_S, "LEO KUROSHITA", C_HI) + 8, 72, &FONT_S, "@KUROGEDELIC", C_AMB);
            cv_text(4, 88, &FONT_S, "H\xDCGELTON INSTRUMENTS", C_HI);   /* Latin-1 U-umlaut */
            cv_text(4, 104, &FONT_S, "HUGELTON.COM", C_AMB);
            cv_text(4, 119, &FONT_S, "GPL-3.0, NO WARRANTY", C_HI);
            cv_text(4, 132, &FONT_S, "GITHUB.COM/HUGELTON/FELUCCA", C_AMB);
            cv_text(4, 146, &FONT_S, "FONT: TERMINUS (OFL)", C_DIM);
            cv_text(4, 159, &FONT_S, "SAMPLES: VERSILIAN (CC0)", C_DIM);
            cv_text(4, 172, &FONT_S, "+ H\xDCGELTON SAMPLE PACK", C_DIM);
            cv_text(4, 185, &FONT_S, "PHASE: CRISPYZEBRA (GPL)", C_DIM);
            cv_text(4, 198, &FONT_S, "VOICE: REF. KLATTSCH (MIT)", C_DIM);
        } else {
            for (i = 0; i < nmi; i++) {
                int32_t y = 4 + (int32_t)i * (nmi > 6u ? 20 : 24);
                uint32_t id = ids[i];
                int sel = i == ui.menu_sel;
                if (sel)
                    cv_rect(4, y + 6, 3, 3, C_WHITE);
#if FELUCCA_WORLD
                if (id == MI_LEAVE && wleave_ask)
                    cv_text(14, y, &FONT_S, "LEAVE WORLD? NOT SAVED", sel ? C_WHITE : C_GRAY);
                else
#endif
                cv_text(14, y, &FONT_S, MI_NAME[id], sel ? C_WHITE : C_GRAY);
                if (id == MI_LOWCUT || id == MI_ZOOM)
                    cv_text(90, y, &FONT_S, (id == MI_LOWCUT ? settings.lowcut : settings.zoom) ? "ON" : "OFF", C_HI);
                if (id == MI_COLOR) {
                    uint32_t k;
                    cv_text(90, y, &FONT_S, PALETTES[settings.palette].name, C_HI);
                    for (k = 0; k < 5u; k++)
                        cv_rect(160 + (int32_t)k * 14, y + 3, 10, 10, pal[k]);
                }
            }
            cv_text(4, 170, &FONT_S, "PRESETS MOVE", C_DIM);
            cv_text(4, 188, &FONT_S, "OCT+ OK   OCT- BACK", C_DIM);
        }
        cv_oy = 0;
        cv_blit(0, H_HEAD + 1 + pass * 124u);
    }
}

static void enc_drop(void)                             /* knob turns nobody takes */
{
    uint32_t k;
    for (k = 0; k < NE; k++)
        panel_enc(k);
}

static void menu_close(void)
{
#if FELUCCA_WORLD
    wleave_ask = 0;
#endif
    settings_save();                                   /* palette / panel table, if changed */
    ui.menu = 0;
    ui.force = 1;
    go_home();
}

/* menu: PRESETS moves, OCT+ confirms, OCT- cancels (ABOUT -> list -> close) */
static void menu_input(uint32_t pressed)
{
    int32_t s;
    uint8_t ids[MI_COUNT];
    uint32_t ok = (pressed >> panel.btn[B_OCTUP]) & 1u, back = (pressed >> panel.btn[B_OCTDN]) & 1u, nmi, item;
    if (back) {
        if (ui.menu == 2)
            ui.menu = 1, ui.force = 1;
        else
            menu_close();
        return;
    }
    nmi = menu_items(ids);
    if (ui.menu_sel >= nmi)
        ui.menu_sel = 0;
    if ((s = panel_enc(EN_PRESET)) != 0 && ui.menu == 1)
        ui.menu_sel = (uint8_t)((ui.menu_sel + (s > 0 ? 1u : nmi - 1u)) % nmi);
    item = ids[ui.menu_sel];
    s = panel_enc(EN_K1);
    if (s != 0 && ui.menu == 1 && item == MI_COLOR) {
        settings.palette = (settings.palette + (s > 0 ? 1u : NPALETTES - 1u)) % NPALETTES;
        palette_set(settings.palette);              /* (the menu signature redraws) */
    }
    if ((s != 0 || ok) && ui.menu == 1 && (item == MI_LOWCUT || item == MI_ZOOM)) {
        /* KNOB 1: right = ON, left = OFF; OCT+ toggles */
        uint32_t *v = item == MI_LOWCUT ? &settings.lowcut : &settings.zoom;
        *v = s > 0 ? 1u : s < 0 ? 0u : !*v;
        fx_lowcut = (uint8_t)(settings.lowcut != 0);
        ok = 0;
    }
    if (ok && ui.menu == 1) {
        switch (item) {
        case MI_COLOR:                                 /* OCT+ steps through the palettes too */
            settings.palette = (settings.palette + 1u) % NPALETTES;
            palette_set(settings.palette);
            break;
        case MI_PANEL:
            panel_setup();
            ui.force = 1;
            break;
        case MI_ABOUT:
            ui.menu = 2;
            ui.force = 1;
            break;
#if FELUCCA_WORLD
        case MI_PLAY:
        case MI_LEAVE:
            if (item == MI_LEAVE && !wleave_ask && play_unsaved()) {
                wleave_ask = 1;                         /* (Phase 14: unsaved edits or loop: OK again leaves) */
                ui.force = 1;
                break;
            }
            menu_close();
            play_menu(item == MI_LEAVE);                /* (ui_play.c) */
            break;
#endif
        default:
            menu_close();
            break;
        }
    }
    enc_drop();                                        /* swallow the rest while the menu is up */
}

