/*
 * settings_dialog.c — modal settings overlay for scrcpy side panel
 */

#include "settings_dialog.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../util/log.h"

/* -----------------------------------------------------------------------
 * Palette
 * --------------------------------------------------------------------- */
static const SDL_Color COL_OVERLAY  = {  0,   0,   0, 160};
static const SDL_Color COL_CARD     = { 28,  28,  38, 245};
static const SDL_Color COL_TITLE_BG = { 20,  90, 190, 255};
static const SDL_Color COL_SEL      = { 40, 130, 220, 255};
static const SDL_Color COL_NORM     = { 50,  50,  70, 255};
static const SDL_Color COL_HOVER    = { 70,  70,  95, 255};
static const SDL_Color COL_TEXT     = {230, 230, 240, 255};
static const SDL_Color COL_MUTED    = {140, 140, 160, 255};
static const SDL_Color COL_SAVE     = { 30, 160,  70, 255};
static const SDL_Color COL_CANCEL   = {160,  50,  50, 255};
static const SDL_Color COL_BORDER   = { 60,  60,  90, 255};

static void set_color(SDL_Renderer *r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h) {
    SDL_Rect rc = {x,y,w,h}; SDL_RenderFillRect(r, &rc);
}
static void draw_rect_col(SDL_Renderer *r, SDL_Color c, int x, int y, int w, int h) {
    set_color(r, c);
    SDL_Rect rc = {x,y,w,h}; SDL_RenderDrawRect(r, &rc);
}

static void fill_rounded(SDL_Renderer *r, SDL_Color c, int x, int y, int w, int h, int rad) {
    set_color(r, c);
    int rr = (rad < w/2 && rad < h/2) ? rad : 4;
    fill_rect(r, x+rr,    y,      w-2*rr, h);
    fill_rect(r, x,       y+rr,   rr,     h-2*rr);
    fill_rect(r, x+w-rr,  y+rr,   rr,     h-2*rr);
    for (int i = 0; i < rr; i++) {
        int len = (int)((float)rr - sqrtf((float)((rr-i)*(rr-i))) + 0.5f);
        if (len < 0) len = 0;
        fill_rect(r, x+rr-len,            y+i,       len, 1);
        fill_rect(r, x+w-rr,               y+i,       len, 1);
        fill_rect(r, x+rr-len,            y+h-1-i,   len, 1);
        fill_rect(r, x+w-rr,               y+h-1-i,  len, 1);
    }
}

/* Render a UTF-8 string with TTF_Font; returns rendered width */
static int render_text(SDL_Renderer *r, TTF_Font *font,
                        SDL_Color col, const char *text,
                        int x, int y, bool centered, int ref_w) {
    if (!font || !text) return 0;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, col);
    if (!surf) return 0;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    int tw = surf->w, th = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return 0;
    int rx = centered ? x + (ref_w - tw)/2 : x;
    SDL_Rect dst = {rx, y - th/2, tw, th};
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    return tw;
}

/* -----------------------------------------------------------------------
 * Layout constants
 * --------------------------------------------------------------------- */
#define DLG_W        420
#define DLG_H        460
#define ROW_H         36
#define OPT_W         76
#define OPT_H         34
#define SECTION_GAP   14
#define TITLE_H       44
#define PAD           20
#define BTN_H         40

/* -----------------------------------------------------------------------
 * Init / Destroy
 * --------------------------------------------------------------------- */
bool
sc_settings_dialog_init(sc_settings_dialog *sd,
                        const sc_settings_dialog_params *params) {
    memset(sd, 0, sizeof(*sd));
    sd->renderer         = params->renderer;
    sd->font             = params->font;
    sd->font_bold        = params->font_bold ? params->font_bold : params->font;
    sd->device_cfg       = params->device_cfg;
    sd->on_save_restart  = params->on_save_restart;
    sd->userdata         = params->userdata;
    sd->visible          = false;
    sd->hover_section    = -1;
    sd->hover_index      = -1;
    return true;
}

void
sc_settings_dialog_destroy(sc_settings_dialog *sd) { (void)sd; }

/* -----------------------------------------------------------------------
 * Show / Hide
 * --------------------------------------------------------------------- */
static int find_idx(const int *arr, int n, int val) {
    for (int i=0; i<n; i++) if (arr[i]==val) return i;
    return 0;
}
static int find_str_idx(const char **arr, int n, const char *val) {
    for (int i=0; i<n; i++) if (strcmp(arr[i],val)==0) return i;
    return 1; /* default 8M */
}

void sc_settings_dialog_show(sc_settings_dialog *sd) {
    if (sd->device_cfg) {
        sd->sel_fps     = find_idx(DC_FPS_OPTIONS, DC_FPS_COUNT,
                                   sd->device_cfg->fps);
        sd->sel_res     = find_idx(DC_RES_OPTIONS, DC_RES_COUNT,
                                   sd->device_cfg->resolution);
        sd->sel_bitrate = find_str_idx(DC_BITRATE_OPTIONS,
                                       DC_BITRATE_COUNT,
                                       sd->device_cfg->bitrate);
        sd->sel_audio   = sd->device_cfg->audio;
    }
    sd->visible = true;
}

void sc_settings_dialog_hide(sc_settings_dialog *sd) { sd->visible = false; }

/* -----------------------------------------------------------------------
 * Render
 * --------------------------------------------------------------------- */

/* Returns rect of option chip [section][idx] */
static SDL_Rect
chip_rect(int dlg_x, int dlg_y, int section, int idx) {
    /* sections: 0=fps, 1=res, 2=bitrate, 3=audio */
    static const int counts[4] = {DC_FPS_COUNT, DC_RES_COUNT,
                                   DC_BITRATE_COUNT, 2};
    int row_y = dlg_y + TITLE_H + PAD
                + section * (ROW_H + OPT_H + SECTION_GAP)
                + ROW_H;
    int total_w = counts[section] * OPT_W + (counts[section]-1) * 6;
    int start_x = dlg_x + PAD + (DLG_W - 2*PAD - total_w)/2;
    int x = start_x + idx * (OPT_W + 6);
    return (SDL_Rect){x, row_y, OPT_W, OPT_H};
}

static SDL_Rect btn_save_rect(int dlg_x, int dlg_y) {
    return (SDL_Rect){dlg_x + PAD, dlg_y + DLG_H - PAD - BTN_H,
                      (DLG_W - 3*PAD)/2, BTN_H};
}
static SDL_Rect btn_cancel_rect(int dlg_x, int dlg_y) {
    SDL_Rect s = btn_save_rect(dlg_x, dlg_y);
    return (SDL_Rect){s.x + s.w + PAD, s.y, s.w, s.h};
}

void
sc_settings_dialog_render(sc_settings_dialog *sd, int win_w, int win_h) {
    if (!sd->visible) return;
    SDL_Renderer *r = sd->renderer;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* Dimmed overlay */
    set_color(r, COL_OVERLAY);
    fill_rect(r, 0, 0, win_w, win_h);

    /* Dialog card */
    int dlg_x = (win_w - DLG_W) / 2;
    int dlg_y = (win_h - DLG_H) / 2;
    fill_rounded(r, COL_CARD, dlg_x, dlg_y, DLG_W, DLG_H, 12);
    draw_rect_col(r, COL_BORDER, dlg_x, dlg_y, DLG_W, DLG_H);

    /* Title bar */
    fill_rounded(r, COL_TITLE_BG, dlg_x, dlg_y, DLG_W, TITLE_H, 12);
    fill_rect(r, dlg_x, dlg_y + TITLE_H - 12, DLG_W, 12);
    render_text(r, sd->font_bold, COL_TEXT,
                sd->device_cfg && sd->device_cfg->device_name[0]
                    ? sd->device_cfg->device_name
                    : "Configurações do Dispositivo",
                dlg_x, dlg_y + TITLE_H/2, true, DLG_W);

    /* Section labels + chips */
    static const char *labels[4] = {"FPS da Transmissão",
                                     "Resolução",
                                     "Bitrate",
                                     "Áudio para PC"};

    for (int sec = 0; sec < 4; sec++) {
        int lbl_y = dlg_y + TITLE_H + PAD
                    + sec * (ROW_H + OPT_H + SECTION_GAP)
                    + ROW_H/2;
        render_text(r, sd->font, COL_MUTED, labels[sec],
                    dlg_x + PAD, lbl_y, false, 0);

        int count = (sec == 3) ? 2
                  : (sec == 0) ? DC_FPS_COUNT
                  : (sec == 1) ? DC_RES_COUNT
                  :              DC_BITRATE_COUNT;

        for (int i = 0; i < count; i++) {
            SDL_Rect cr = chip_rect(dlg_x, dlg_y, sec, i);

            bool selected = false;
            const char *chip_lbl = NULL;
            char tmp[16];

            if (sec == 0) {
                selected = (i == sd->sel_fps);
                snprintf(tmp, sizeof(tmp), "%d", DC_FPS_OPTIONS[i]);
                chip_lbl = tmp;
            } else if (sec == 1) {
                selected = (i == sd->sel_res);
                snprintf(tmp, sizeof(tmp), "%dp", DC_RES_OPTIONS[i]);
                chip_lbl = tmp;
            } else if (sec == 2) {
                selected = (i == sd->sel_bitrate);
                chip_lbl = DC_BITRATE_OPTIONS[i];
            } else {
                selected = (i == 0) ? sd->sel_audio : !sd->sel_audio;
                chip_lbl = (i == 0) ? "Sim" : "Não";
            }

            bool hovered = (sd->hover_section == sec && sd->hover_index == i);
            SDL_Color bg = selected ? COL_SEL : hovered ? COL_HOVER : COL_NORM;
            fill_rounded(r, bg, cr.x, cr.y, cr.w, cr.h, 6);
            if (selected)
                draw_rect_col(r, COL_TEXT, cr.x, cr.y, cr.w, cr.h);

            render_text(r, sd->font, COL_TEXT, chip_lbl,
                        cr.x, cr.y + cr.h/2, true, cr.w);
        }
    }

    /* Separator */
    set_color(r, COL_BORDER);
    SDL_RenderDrawLine(r, dlg_x+PAD, dlg_y+DLG_H-PAD-BTN_H-10,
                          dlg_x+DLG_W-PAD, dlg_y+DLG_H-PAD-BTN_H-10);

    /* Buttons */
    SDL_Rect bs = btn_save_rect(dlg_x, dlg_y);
    SDL_Rect bc = btn_cancel_rect(dlg_x, dlg_y);
    fill_rounded(r, sd->hover_save   ? COL_HOVER : COL_SAVE,
                 bs.x, bs.y, bs.w, bs.h, 8);
    fill_rounded(r, sd->hover_cancel ? COL_HOVER : COL_CANCEL,
                 bc.x, bc.y, bc.w, bc.h, 8);
    render_text(r, sd->font_bold, COL_TEXT, "Salvar e Reiniciar",
                bs.x, bs.y + bs.h/2, true, bs.w);
    render_text(r, sd->font, COL_TEXT, "Cancelar",
                bc.x, bc.y + bc.h/2, true, bc.w);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

/* -----------------------------------------------------------------------
 * Event handling
 * --------------------------------------------------------------------- */
bool
sc_settings_dialog_handle_event(sc_settings_dialog *sd,
                                const SDL_Event *event,
                                int win_w, int win_h) {
    if (!sd->visible) return false;

    int dlg_x = (win_w - DLG_W) / 2;
    int dlg_y = (win_h - DLG_H) / 2;

    if (event->type == SDL_MOUSEMOTION) {
        int mx = event->motion.x, my = event->motion.y;
        sd->hover_section = -1; sd->hover_index = -1;
        sd->hover_save = sd->hover_cancel = false;

        for (int sec = 0; sec < 4; sec++) {
            int count = (sec == 3) ? 2
                      : (sec == 0) ? DC_FPS_COUNT
                      : (sec == 1) ? DC_RES_COUNT
                      :              DC_BITRATE_COUNT;
            for (int i = 0; i < count; i++) {
                SDL_Rect cr = chip_rect(dlg_x, dlg_y, sec, i);
                if (mx >= cr.x && mx < cr.x+cr.w &&
                    my >= cr.y && my < cr.y+cr.h) {
                    sd->hover_section = sec;
                    sd->hover_index   = i;
                }
            }
        }
        SDL_Rect bs = btn_save_rect(dlg_x, dlg_y);
        SDL_Rect bc = btn_cancel_rect(dlg_x, dlg_y);
        sd->hover_save   = (mx>=bs.x && mx<bs.x+bs.w && my>=bs.y && my<bs.y+bs.h);
        sd->hover_cancel = (mx>=bc.x && mx<bc.x+bc.w && my>=bc.y && my<bc.y+bc.h);
        return true;
    }

    if (event->type == SDL_MOUSEBUTTONDOWN &&
        event->button.button == SDL_BUTTON_LEFT) {
        int mx = event->button.x, my = event->button.y;

        /* Click outside dialog → cancel */
        if (mx < dlg_x || mx > dlg_x+DLG_W ||
            my < dlg_y || my > dlg_y+DLG_H) {
            sd->visible = false;
            return true;
        }

        /* Chips */
        for (int sec = 0; sec < 4; sec++) {
            int count = (sec == 3) ? 2
                      : (sec == 0) ? DC_FPS_COUNT
                      : (sec == 1) ? DC_RES_COUNT
                      :              DC_BITRATE_COUNT;
            for (int i = 0; i < count; i++) {
                SDL_Rect cr = chip_rect(dlg_x, dlg_y, sec, i);
                if (mx >= cr.x && mx < cr.x+cr.w &&
                    my >= cr.y && my < cr.y+cr.h) {
                    switch (sec) {
                        case 0: sd->sel_fps     = i; break;
                        case 1: sd->sel_res     = i; break;
                        case 2: sd->sel_bitrate = i; break;
                        case 3: sd->sel_audio   = (i == 0); break;
                    }
                }
            }
        }

        /* Save button */
        SDL_Rect bs = btn_save_rect(dlg_x, dlg_y);
        if (mx>=bs.x && mx<bs.x+bs.w && my>=bs.y && my<bs.y+bs.h) {
            /* Commit to live config */
            if (sd->device_cfg) {
                sd->device_cfg->fps        = DC_FPS_OPTIONS[sd->sel_fps];
                sd->device_cfg->resolution = DC_RES_OPTIONS[sd->sel_res];
                strncpy(sd->device_cfg->bitrate,
                        DC_BITRATE_OPTIONS[sd->sel_bitrate],
                        sizeof(sd->device_cfg->bitrate) - 1);
                sd->device_cfg->audio = sd->sel_audio;
                sc_device_config_save(sd->device_cfg);
            }
            sd->visible = false;
            if (sd->on_save_restart)
                sd->on_save_restart(sd->userdata, sd->device_cfg);
        }

        /* Cancel button */
        SDL_Rect bc = btn_cancel_rect(dlg_x, dlg_y);
        if (mx>=bc.x && mx<bc.x+bc.w && my>=bc.y && my<bc.y+bc.h)
            sd->visible = false;

        return true;
    }

    /* ESC closes */
    if (event->type == SDL_KEYDOWN &&
        event->key.keysym.sym == SDLK_ESCAPE) {
        sd->visible = false;
        return true;
    }

    return true; /* consume all events while visible */
}
