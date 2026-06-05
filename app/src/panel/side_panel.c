/*
 * side_panel.c — retractable right-side toolbar for scrcpy (SDL2)
 *
 * Drawn entirely with SDL2 primitives (no external image deps).
 * Requires SDL2_ttf only for text labels; set font=NULL to run without it.
 */

#include "side_panel.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../util/log.h"

/* -----------------------------------------------------------------------
 * Colour palette
 * --------------------------------------------------------------------- */
#define COL_BG          { 30,  30,  38, 220}   /* dark translucent panel  */
#define COL_BTN_NORMAL  { 50,  50,  65, 255}   /* button default          */
#define COL_BTN_HOVER   { 80,  80, 110, 255}   /* button hovered          */
#define COL_BTN_ACTIVE  { 40, 130, 220, 255}   /* button pressed/toggled  */
#define COL_TAB         { 40, 130, 220, 200}   /* pull-tab stripe         */
#define COL_TEXT        {230, 230, 240, 255}
#define COL_SEPARATOR   { 60,  60,  75, 255}
#define COL_ICON        {200, 210, 240, 255}

static const SDL_Color c_bg        = COL_BG;
static const SDL_Color c_btn_norm  = COL_BTN_NORMAL;
static const SDL_Color c_btn_hover = COL_BTN_HOVER;
static const SDL_Color c_btn_act   = COL_BTN_ACTIVE;
static const SDL_Color c_tab       = COL_TAB;
static const SDL_Color c_text      = COL_TEXT;
static const SDL_Color c_icon      = COL_ICON;
static const SDL_Color c_sep       = COL_SEPARATOR;

/* -----------------------------------------------------------------------
 * Button descriptor
 * --------------------------------------------------------------------- */
typedef struct {
    sp_button_id id;
    const char  *label;      /* short label shown below icon */
} sp_btn_info;

static const sp_btn_info sp_buttons[SP_BTN_COUNT] = {
    {SP_BTN_SCREENSHOT,   "Print"},
    {SP_BTN_AUDIO_TOGGLE, "Audio"},
    {SP_BTN_VOL_UP,       "Vol+"},
    {SP_BTN_VOL_DOWN,     "Vol-"},
    {SP_BTN_BACK,         "Back"},
    {SP_BTN_HOME,         "Home"},
    {SP_BTN_RECENTS,      "Apps"},
    {SP_BTN_SETTINGS,     "Config"},
};

/* -----------------------------------------------------------------------
 * Small SDL_RenderFill helpers (no SDL_gfx required)
 * --------------------------------------------------------------------- */
static void set_color(SDL_Renderer *r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h) {
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void draw_rect(SDL_Renderer *r, int x, int y, int w, int h) {
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderDrawRect(r, &rect);
}

/* Rounded-rect approximation with small corner quads */
static void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius) {
    if (radius <= 0) { fill_rect(r, x, y, w, h); return; }
    int r2 = (radius < w/2 && radius < h/2) ? radius : 4;
    fill_rect(r, x + r2,    y,      w - 2*r2, h);
    fill_rect(r, x,         y + r2, r2,        h - 2*r2);
    fill_rect(r, x + w - r2, y + r2, r2,       h - 2*r2);
    /* corners */
    for (int i = 0; i < r2; i++) {
        int len = (int)(r2 - sqrtf((float)((r2-i)*(r2-i))) + 0.5f);
        fill_rect(r, x + r2 - len,             y + i,      len, 1);
        fill_rect(r, x + w - r2,               y + i,      len, 1);
        fill_rect(r, x + r2 - len,             y + h-1-i,  len, 1);
        fill_rect(r, x + w - r2,               y + h-1-i,  len, 1);
    }
}

/* -----------------------------------------------------------------------
 * Icon draw routines — pure SDL2 lines & rects
 * --------------------------------------------------------------------- */
static void draw_icon_screenshot(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz / 2;
    /* Outer rectangle (phone/screen frame) */
    draw_rect(r, cx-s, cy-s, sz, sz);
    /* Inner circle (lens) */
    int ra = s/3;
    for (int a = 0; a < 16; a++) {
        float angle  = (float)a * 3.14159f * 2 / 16;
        float angle2 = (float)(a+1) * 3.14159f * 2 / 16;
        SDL_RenderDrawLine(r,
            cx + (int)(ra*cosf(angle)),  cy + (int)(ra*sinf(angle)),
            cx + (int)(ra*cosf(angle2)), cy + (int)(ra*sinf(angle2)));
    }
    /* Shutter line */
    SDL_RenderDrawLine(r, cx-s+3, cy-s+3, cx+s-3, cy-s+3);
}

static void draw_icon_audio_on(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz/2;
    /* Speaker box */
    int bx = cx - s, bw = sz/3;
    SDL_Rect box = {bx, cy - sz/4, bw, sz/2};
    SDL_RenderDrawRect(r, &box);
    /* Cone */
    SDL_RenderDrawLine(r, bx,    cy-sz/4, cx,    cy-s);
    SDL_RenderDrawLine(r, bx,    cy+sz/4, cx,    cy+s);
    SDL_RenderDrawLine(r, cx,    cy-s,    cx,    cy+s);
    /* Sound waves */
    SDL_RenderDrawLine(r, cx+3, cy-sz/4, cx+6, cy-sz/3);
    SDL_RenderDrawLine(r, cx+6, cy-sz/3, cx+6, cy+sz/3);
    SDL_RenderDrawLine(r, cx+6, cy+sz/3, cx+3, cy+sz/4);
}

static void draw_icon_audio_off(SDL_Renderer *r, int cx, int cy, int sz) {
    draw_icon_audio_on(r, cx, cy, sz);
    /* Red X */
    int x1 = cx + sz/4, y1 = cy - sz/3;
    int x2 = cx + sz/2, y2 = cy + sz/3;
    SDL_RenderDrawLine(r, x1, y1, x2, y2);
    SDL_RenderDrawLine(r, x2, y1, x1, y2);
}

static void draw_icon_vol_up(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz/3;
    SDL_RenderDrawLine(r, cx-s, cy,   cx+s, cy);
    SDL_RenderDrawLine(r, cx,   cy-s, cx,   cy+s);
    /* Speaker box */
    SDL_Rect box = {cx-sz/2, cy-sz/6, sz/3, sz/3};
    SDL_RenderDrawRect(r, &box);
}

static void draw_icon_vol_down(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz/3;
    SDL_RenderDrawLine(r, cx-s, cy, cx+s, cy);
    SDL_Rect box = {cx-sz/2, cy-sz/6, sz/3, sz/3};
    SDL_RenderDrawRect(r, &box);
}

static void draw_icon_back(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz/2;
    /* Left-pointing arrow */
    SDL_RenderDrawLine(r, cx+s, cy-s, cx-s, cy);
    SDL_RenderDrawLine(r, cx-s, cy,   cx+s, cy+s);
    SDL_RenderDrawLine(r, cx-s, cy,   cx+s, cy);
}

static void draw_icon_home(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz/2;
    /* Triangle roof */
    SDL_RenderDrawLine(r, cx,    cy-s, cx-s, cy);
    SDL_RenderDrawLine(r, cx,    cy-s, cx+s, cy);
    /* Walls + door */
    SDL_RenderDrawLine(r, cx-s,  cy,   cx-s, cy+s);
    SDL_RenderDrawLine(r, cx+s,  cy,   cx+s, cy+s);
    SDL_RenderDrawLine(r, cx-s,  cy+s, cx+s, cy+s);
    SDL_Rect door = {cx - sz/8, cy + sz/8, sz/4, sz/3};
    SDL_RenderDrawRect(r, &door);
}

static void draw_icon_recents(SDL_Renderer *r, int cx, int cy, int sz) {
    int s = sz/2, gap = 4;
    draw_rect(r, cx-s,      cy-s,     sz-gap, sz-gap);
    draw_rect(r, cx-s+gap,  cy-s+gap, sz-gap, sz-gap);
}

static void draw_icon_settings(SDL_Renderer *r, int cx, int cy, int sz) {
    /* Gear-ish: circle with 6 tooth lines */
    int ra = sz/4, rb = sz/2;
    for (int t = 0; t < 6; t++) {
        float a = (float)t * 3.14159f / 3;
        SDL_RenderDrawLine(r,
            cx + (int)(ra*cosf(a)), cy + (int)(ra*sinf(a)),
            cx + (int)(rb*cosf(a)), cy + (int)(rb*sinf(a)));
    }
    /* Inner circle outline */
    for (int a = 0; a < 20; a++) {
        float a1 = (float)a * 3.14159f * 2 / 20;
        float a2 = (float)(a+1) * 3.14159f * 2 / 20;
        SDL_RenderDrawLine(r,
            cx+(int)(ra*cosf(a1)), cy+(int)(ra*sinf(a1)),
            cx+(int)(ra*cosf(a2)), cy+(int)(ra*sinf(a2)));
    }
}

typedef void (*icon_draw_fn)(SDL_Renderer*, int, int, int);
static const icon_draw_fn icon_fns[SP_BTN_COUNT] = {
    draw_icon_screenshot,
    draw_icon_audio_on,   /* dynamic: off variant called if !audio_on */
    draw_icon_vol_up,
    draw_icon_vol_down,
    draw_icon_back,
    draw_icon_home,
    draw_icon_recents,
    draw_icon_settings,
};

/* -----------------------------------------------------------------------
 * Geometry helpers
 * --------------------------------------------------------------------- */
static SDL_Rect
btn_rect(const sc_side_panel *sp, int idx) {
    int panel_x = sp->current_x;
    int y = SP_BTN_PADDING + idx * (SP_BTN_H + SP_BTN_PADDING);
    return (SDL_Rect){panel_x + SP_BTN_PADDING, y,
                      SP_WIDTH_EXPANDED - 2*SP_BTN_PADDING, SP_BTN_H};
}

static SDL_Rect
tab_rect(const sc_side_panel *sp) {
    int panel_x = sp->current_x;
    return (SDL_Rect){panel_x - SP_WIDTH_TAB, 0,
                      SP_WIDTH_TAB, sp->win_h};
}

static int
hit_button(const sc_side_panel *sp, int mx, int my) {
    if (!sp->expanded) return SP_BTN_TOGGLE;
    SDL_Rect tab = tab_rect(sp);
    if (mx >= tab.x && mx < tab.x + tab.w &&
        my >= tab.y && my < tab.y + tab.h) return SP_BTN_TOGGLE;
    for (int i = 0; i < SP_BTN_COUNT; i++) {
        SDL_Rect r = btn_rect(sp, i);
        if (mx >= r.x && mx < r.x+r.w && my >= r.y && my < r.y+r.h)
            return i;
    }
    return -2; /* no hit */
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
bool
sc_side_panel_init(sc_side_panel *sp, const sc_side_panel_params *params) {
    memset(sp, 0, sizeof(*sp));
    sp->renderer        = params->renderer;
    sp->font            = params->font;
    sp->on_screenshot   = params->on_screenshot;
    sp->on_audio_toggle = params->on_audio_toggle;
    sp->on_volume       = params->on_volume;
    sp->on_nav          = params->on_nav;
    sp->on_settings     = params->on_settings;
    sp->userdata        = params->userdata;
    sp->audio_on        = params->device_cfg ? params->device_cfg->audio : true;
    sp->expanded        = false;
    return true;
}

void
sc_side_panel_destroy(sc_side_panel *sp) {
    (void)sp;
}

void
sc_side_panel_update(sc_side_panel *sp, int win_w, int win_h) {
    sp->win_w = win_w;
    sp->win_h = win_h;
    sp->target_x = sp->expanded ? win_w - SP_WIDTH_EXPANDED : win_w;
    /* Animate */
    int diff = sp->target_x - sp->current_x;
    if (abs(diff) <= SP_ANIM_SPEED) sp->current_x = sp->target_x;
    else sp->current_x += (diff > 0) ? SP_ANIM_SPEED : -SP_ANIM_SPEED;
}

void
sc_side_panel_render(sc_side_panel *sp) {
    SDL_Renderer *r = sp->renderer;
    int panel_x = sp->current_x;
    int W = SP_WIDTH_EXPANDED;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* --- draw panel background ---------------------------------------- */
    set_color(r, c_bg);
    fill_rect(r, panel_x, 0, W, sp->win_h);

    /* left border line */
    set_color(r, c_sep);
    SDL_RenderDrawLine(r, panel_x, 0, panel_x, sp->win_h);

    /* --- draw pull tab ------------------------------------------------- */
    SDL_Rect tab = tab_rect(sp);
    set_color(r, c_tab);
    fill_rounded_rect(r, tab.x, tab.y + sp->win_h/2 - 40,
                      tab.w, 80, 6);
    /* arrow on tab */
    set_color(r, c_text);
    int tx = tab.x + tab.w/2;
    int ty = tab.y + sp->win_h/2;
    int arrow = 5;
    if (sp->expanded) {
        /* right-pointing > */
        SDL_RenderDrawLine(r, tx-arrow/2, ty-arrow, tx+arrow/2, ty);
        SDL_RenderDrawLine(r, tx+arrow/2, ty,       tx-arrow/2, ty+arrow);
    } else {
        /* left-pointing < */
        SDL_RenderDrawLine(r, tx+arrow/2, ty-arrow, tx-arrow/2, ty);
        SDL_RenderDrawLine(r, tx-arrow/2, ty,       tx+arrow/2, ty+arrow);
    }

    /* --- draw buttons -------------------------------------------------- */
    if (sp->current_x < sp->win_w) {
        int mx, my;
        Uint32 mouse_state = SDL_GetMouseState(&mx, &my);
        (void)mouse_state;

        for (int i = 0; i < SP_BTN_COUNT; i++) {
            SDL_Rect br = btn_rect(sp, i);

            /* skip if off screen */
            if (br.y + br.h < 0 || br.y > sp->win_h) continue;

            /* hover detection */
            bool hovered = (mx >= br.x && mx < br.x+br.w &&
                            my >= br.y && my < br.y+br.h);
            bool active = (i == SP_BTN_AUDIO_TOGGLE && !sp->audio_on);

            set_color(r, active ? c_btn_act : hovered ? c_btn_hover : c_btn_norm);
            fill_rounded_rect(r, br.x, br.y, br.w, br.h, 8);

            /* icon */
            set_color(r, c_icon);
            int icon_sz = 20;
            int cx = br.x + 28;
            int cy = br.y + br.h/2;
            if (i == SP_BTN_AUDIO_TOGGLE && !sp->audio_on)
                draw_icon_audio_off(r, cx, cy, icon_sz);
            else
                icon_fns[i](r, cx, cy, icon_sz);

            /* label */
            if (sp->font) {
                SDL_Color lc = c_text;
                SDL_Surface *surf = TTF_RenderUTF8_Blended(
                    sp->font, sp_buttons[i].label, lc);
                if (surf) {
                    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
                    if (tex) {
                        SDL_Rect dst = {br.x + 52, cy - surf->h/2,
                                        surf->w, surf->h};
                        SDL_RenderCopy(r, tex, NULL, &dst);
                        SDL_DestroyTexture(tex);
                    }
                    SDL_FreeSurface(surf);
                }
            }

            /* separator */
            if (i < SP_BTN_COUNT - 1 &&
                (i == SP_BTN_AUDIO_TOGGLE || i == SP_BTN_VOL_DOWN ||
                 i == SP_BTN_RECENTS)) {
                set_color(r, c_sep);
                int sep_y = br.y + br.h + SP_BTN_PADDING/2;
                SDL_RenderDrawLine(r, br.x + 4, sep_y, br.x+br.w-4, sep_y);
            }
        }
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

bool
sc_side_panel_handle_event(sc_side_panel *sp, const SDL_Event *event) {
    if (event->type != SDL_MOUSEBUTTONDOWN) return false;
    if (event->button.button != SDL_BUTTON_LEFT) return false;

    int mx = event->button.x, my = event->button.y;

    /* hit-test pull tab even when collapsed */
    SDL_Rect tab = tab_rect(sp);
    if (mx >= tab.x && mx < tab.x + tab.w &&
        my >= tab.y && my < tab.y + tab.h) {
        sp->expanded = !sp->expanded;
        return true;
    }

    /* if panel is completely out of view, nothing to do */
    if (!sp->expanded || sp->current_x >= sp->win_w) return false;

    /* hit inside panel area */
    if (mx < sp->current_x) return false;

    int hit = hit_button(sp, mx, my);
    if (hit < 0) return (mx >= sp->current_x); /* consume click on panel bg */

    switch ((sp_button_id)hit) {
        case SP_BTN_SCREENSHOT:
            if (sp->on_screenshot) sp->on_screenshot(sp->userdata);
            break;
        case SP_BTN_AUDIO_TOGGLE:
            sp->audio_on = !sp->audio_on;
            if (sp->on_audio_toggle)
                sp->on_audio_toggle(sp->userdata, &sp->audio_on);
            break;
        case SP_BTN_VOL_UP:
            if (sp->on_volume) sp->on_volume(sp->userdata, +1);
            break;
        case SP_BTN_VOL_DOWN:
            if (sp->on_volume) sp->on_volume(sp->userdata, -1);
            break;
        case SP_BTN_BACK:
        case SP_BTN_HOME:
        case SP_BTN_RECENTS:
            if (sp->on_nav) sp->on_nav(sp->userdata, (sp_button_id)hit);
            break;
        case SP_BTN_SETTINGS:
            if (sp->on_settings) sp->on_settings(sp->userdata);
            break;
        default: break;
    }
    return true;
}
