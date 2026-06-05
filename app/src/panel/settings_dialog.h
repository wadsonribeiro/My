#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

/*
 * settings_dialog — modal overlay rendered in SDL2
 *
 * Lets the user pick FPS, resolution and bitrate for the current device.
 * On "Save & Restart" it:
 *   1. Writes the updated config to devices_data.json
 *   2. Invokes the restart_cb so scrcpy can relaunch with new params
 */

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "device_config.h"

typedef void (*sd_save_restart_cb)(void *userdata,
                                   const sc_device_config *new_cfg);

typedef struct {
    SDL_Renderer      *renderer;
    TTF_Font          *font;        /* can be NULL */
    TTF_Font          *font_bold;   /* can be NULL */
    sc_device_config  *device_cfg;  /* pointer to live config (will be modified) */
    sd_save_restart_cb on_save_restart;
    void              *userdata;
} sc_settings_dialog_params;

typedef struct sc_settings_dialog sc_settings_dialog;

struct sc_settings_dialog {
    SDL_Renderer      *renderer;
    TTF_Font          *font;
    TTF_Font          *font_bold;
    sc_device_config  *device_cfg;  /* live config, modified in-place on save */

    sd_save_restart_cb on_save_restart;
    void              *userdata;

    bool               visible;

    /* temporary working values (committed only on Save) */
    int   sel_fps;       /* index into DC_FPS_OPTIONS     */
    int   sel_res;       /* index into DC_RES_OPTIONS     */
    int   sel_bitrate;   /* index into DC_BITRATE_OPTIONS */
    bool  sel_audio;

    /* hover state */
    int hover_section;  /* 0=fps, 1=res, 2=bitrate, 3=audio */
    int hover_index;    /* option index within section      */
    bool hover_save;
    bool hover_cancel;
};

bool sc_settings_dialog_init(sc_settings_dialog *sd,
                             const sc_settings_dialog_params *params);
void sc_settings_dialog_destroy(sc_settings_dialog *sd);

void sc_settings_dialog_show(sc_settings_dialog *sd);
void sc_settings_dialog_hide(sc_settings_dialog *sd);

/* Call every frame when visible */
void sc_settings_dialog_render(sc_settings_dialog *sd, int win_w, int win_h);

/* Returns true if event was consumed */
bool sc_settings_dialog_handle_event(sc_settings_dialog *sd,
                                     const SDL_Event *event,
                                     int win_w, int win_h);

#endif /* SETTINGS_DIALOG_H */
