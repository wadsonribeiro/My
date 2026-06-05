#ifndef SIDE_PANEL_H
#define SIDE_PANEL_H

/*
 * side_panel — retractable right-side toolbar rendered in SDL2.
 *
 * Layout (collapsed state):
 *   A small 24-px wide "tab" strip is always visible on the right edge.
 *   Clicking it slides the full panel in/out.
 *
 * Layout (expanded state):
 *   A 200-px wide panel appears from the right edge containing icon buttons.
 */

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "device_config.h"

#define SP_WIDTH_EXPANDED   200
#define SP_WIDTH_TAB         24
#define SP_BTN_H             52
#define SP_BTN_PADDING        6
#define SP_ANIM_SPEED        18  /* px per frame */

/* Button IDs — keep in sync with sp_buttons[] in .c */
typedef enum {
    SP_BTN_TOGGLE = -1,   /* the pull-tab itself */
    SP_BTN_SCREENSHOT = 0,
    SP_BTN_AUDIO_TOGGLE,
    SP_BTN_VOL_UP,
    SP_BTN_VOL_DOWN,
    SP_BTN_BACK,
    SP_BTN_HOME,
    SP_BTN_RECENTS,
    SP_BTN_SETTINGS,
    SP_BTN_COUNT
} sp_button_id;

typedef struct sc_side_panel sc_side_panel;

/* Callback types */
typedef void (*sp_screenshot_cb)(void *userdata);
typedef void (*sp_audio_toggle_cb)(void *userdata, bool *audio_on);
typedef void (*sp_volume_cb)(void *userdata, int delta);   /* delta = +1 / -1 */
typedef void (*sp_nav_cb)(void *userdata, sp_button_id btn);
typedef void (*sp_settings_cb)(void *userdata);

typedef struct {
    SDL_Renderer          *renderer;
    TTF_Font              *font;          /* may be NULL — labels disabled */
    sc_device_config      *device_cfg;   /* pointer to live config */

    sp_screenshot_cb       on_screenshot;
    sp_audio_toggle_cb     on_audio_toggle;
    sp_volume_cb           on_volume;
    sp_nav_cb              on_nav;
    sp_settings_cb         on_settings;
    void                  *userdata;
} sc_side_panel_params;

struct sc_side_panel {
    SDL_Renderer *renderer;
    TTF_Font     *font;

    /* geometry */
    int win_w, win_h;     /* updated each frame */
    int current_x;        /* left edge of panel (animated) */
    int target_x;         /* either fully open or tab-only */
    bool expanded;

    /* audio state (mirrored from config) */
    bool audio_on;

    /* callbacks */
    sp_screenshot_cb   on_screenshot;
    sp_audio_toggle_cb on_audio_toggle;
    sp_volume_cb       on_volume;
    sp_nav_cb          on_nav;
    sp_settings_cb     on_settings;
    void              *userdata;
};

bool sc_side_panel_init(sc_side_panel *sp,
                        const sc_side_panel_params *params);
void sc_side_panel_destroy(sc_side_panel *sp);

/* Call every frame before sc_side_panel_render */
void sc_side_panel_update(sc_side_panel *sp, int win_w, int win_h);

/* Render panel on top of the video frame */
void sc_side_panel_render(sc_side_panel *sp);

/* Feed SDL mouse/touch events; returns true if event was consumed */
bool sc_side_panel_handle_event(sc_side_panel *sp, const SDL_Event *event);

#endif /* SIDE_PANEL_H */
