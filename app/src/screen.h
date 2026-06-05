#ifndef SC_SCREEN_H
#define SC_SCREEN_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "controller.h"
#include "coords.h"
#include "disconnect.h"
#include "fps_counter.h"
#include "graphic_buffer.h"
#include "orientation.h"
#include "sidebar.h"          // ← ADICIONADO
#include "trait/frame_sink.h"
#include "util/thread.h"
#include "util/tick.h"

struct sc_screen {
    struct sc_frame_sink frame_sink; // must be the first field

    struct sc_controller *controller;
    struct sc_file_pusher *file_pusher;
    const struct sc_screen_capture *screen_capture;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    struct sc_size frame_size;
    struct sc_size content_size; // the part of the window containing the device
                                 // screen (window minus black borders)
    enum sc_orientation orientation;
    bool mipmaps;

    struct sc_sidebar sidebar;   // ← ADICIONADO

    bool windowed;    // contain at least one resize from the initial size
    bool fullscreen;
    bool maximized;
    bool minimized; // window is minimized (not rendered)

    bool resize_pending; // resize requested while fullscreen or maximized
    struct sc_size pending_resize;

    bool no_window;

    struct sc_disconnect disconnect;

    bool mouse_capture_key_pressed; // track key pressed during mouse capture

    struct sc_fps_counter fps_counter;

    struct sc_graphic_buffer graphic_buffer;

    const struct sc_screen_ops *ops;
};

struct sc_screen_params {
    struct sc_controller *controller;
    struct sc_file_pusher *file_pusher;
    const struct sc_screen_capture *screen_capture;
    const char *window_title;
    bool windowed;
    struct sc_size frame_size;
    bool always_on_top;
    int16_t window_x; // SDL_WINDOWPOS_UNDEFINED if not set
    int16_t window_y; // SDL_WINDOWPOS_UNDEFINED if not set
    uint16_t window_width;
    uint16_t window_height;
    bool window_borderless;
    enum sc_orientation orientation;
    bool mipmaps;
    bool fullscreen;
    bool no_window;
    bool print_fps;
};

struct sc_screen_ops {
    void (*on_frame_received)(struct sc_screen *screen);
    void (*on_frame_rendered)(struct sc_screen *screen);
};

bool
sc_screen_init(struct sc_screen *screen, const struct sc_screen_params *params);

void
sc_screen_destroy(struct sc_screen *screen);

void
sc_screen_set_ops(struct sc_screen *screen, const struct sc_screen_ops *ops);

// Returns the content size (actual video area, not window size)
struct sc_size
sc_screen_get_content_size(const struct sc_screen *screen);

// Handles SDL events for the screen
bool
sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event);

#endif // SC_SCREEN_H
