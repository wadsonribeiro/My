#include "screen.h"

#include <assert.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "events.h"
#include "icon.h"
#include "options.h"
#include "sidebar.h"
#include "util/log.h"
#include "util/sdl.h"

#define DISPLAY_MARGINS 96

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

static void
set_aspect_ratio(struct sc_screen *screen, struct sc_size content_size) {
    assert(content_size.width && content_size.height);

    if (screen->window_aspect_ratio_lock) {
        float ar = (float) content_size.width / content_size.height;
        bool ok = SDL_SetWindowAspectRatio(screen->window, ar, ar);
        if (!ok) {
            LOGW("Could not set window aspect ratio: %s", SDL_GetError());
        }
    }
}

static inline struct sc_size
get_oriented_size(struct sc_size size, enum sc_orientation orientation) {
    struct sc_size oriented_size;
    if (sc_orientation_is_swap(orientation)) {
        oriented_size.width = size.height;
        oriented_size.height = size.width;
    } else {
        oriented_size.width = size.width;
        oriented_size.height = size.height;
    }
    return oriented_size;
}

static inline bool
is_windowed(struct sc_screen *screen) {
    return !(SDL_GetWindowFlags(screen->window) & (SDL_WINDOW_FULLSCREEN
                                                 | SDL_WINDOW_MINIMIZED
                                                 | SDL_WINDOW_MAXIMIZED));
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static bool
get_preferred_display_bounds(struct sc_size *bounds) {
    SDL_Rect rect;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (!display) {
        LOGW("Could not get primary display: %s", SDL_GetError());
        return false;
    }

    bool ok = SDL_GetDisplayUsableBounds(display, &rect);
    if (!ok) {
        LOGW("Could not get display usable bounds: %s", SDL_GetError());
        return false;
    }

    bounds->width = MAX(0, rect.w - DISPLAY_MARGINS);
    bounds->height = MAX(0, rect.h - DISPLAY_MARGINS);
    return true;
}

static bool
is_optimal_size(struct sc_size current_size, struct sc_size content_size) {
    return current_size.height == (uint32_t) current_size.width
                                * content_size.height / content_size.width
        || current_size.width == (uint32_t) current_size.height
                               * content_size.width / content_size.height;
}

static struct sc_size
get_optimal_size(struct sc_size current_size, struct sc_size content_size,
                 bool within_display_bounds) {
    if (content_size.width == 0 || content_size.height == 0) {
        return current_size;
    }

    struct sc_size window_size;

    struct sc_size display_size;
    if (!within_display_bounds ||
            !get_preferred_display_bounds(&display_size)) {
        window_size = current_size;
    } else {
        window_size.width = MIN(current_size.width, display_size.width);
        window_size.height = MIN(current_size.height, display_size.height);
    }

    if (is_optimal_size(window_size, content_size)) {
        return window_size;
    }

    bool keep_width = (uint32_t) content_size.width * window_size.height
                    > (uint32_t) content_size.height * window_size.width;
    if (keep_width) {
        window_size.height = (uint32_t) content_size.height * window_size.width
                           / content_size.width;
    } else {
        window_size.width = (uint32_t) content_size.width * window_size.height
                          / content_size.height;
    }

    return window_size;
}

static inline struct sc_size
get_initial_optimal_size(struct sc_size content_size, uint16_t req_width,
                         uint16_t req_height) {
    struct sc_size window_size;
    if (!req_width && !req_height) {
        window_size = get_optimal_size(content_size, content_size, true);
    } else {
        if (req_width) {
            window_size.width = req_width;
        } else {
            window_size.width = (uint32_t) req_height * content_size.width
                              / content_size.height;
        }
        if (req_height) {
            window_size.height = req_height;
        } else {
            window_size.height = (uint32_t) req_width * content_size.height
                               / content_size.width;
        }
    }
    return window_size;
}

static inline void
sc_screen_track_resize(struct sc_screen *screen, struct sc_size size) {
    LOGV("Track resize: %" PRIu16 "x%" PRIu16, size.width, size.height);
    screen->resize_tracker.time = sc_tick_now();
    screen->resize_tracker.size = size;
}

static inline bool
sc_screen_is_relative_mode(struct sc_screen *screen) {
    return screen->im.mp && screen->im.mp->relative_mode;
}

static void
compute_content_rect(struct sc_size window_size, struct sc_size content_size,
                     bool is_icon, enum sc_render_fit render_fit,
                     SDL_FRect *rect) {
    // Reserve right side for sidebar — only when not icon mode
    struct sc_size video_area = window_size;
    if (!is_icon) {
        video_area.width = (window_size.width > SIDEBAR_WIDTH)
                         ? window_size.width - SIDEBAR_WIDTH
                         : window_size.width;
    }

    if (is_icon) {
        if (content_size.width <= video_area.width
                && content_size.height <= video_area.height) {
            rect->x = (video_area.width - content_size.width) / 2.f;
            rect->y = (video_area.height - content_size.height) / 2.f;
            rect->w = content_size.width;
            rect->h = content_size.height;
            return;
        }
    } else if (render_fit == SC_RENDER_FIT_UNSCALED) {
        float x = ((float) video_area.width - content_size.width) / 2.f;
        float y = ((float) video_area.height - content_size.height) / 2.f;
        rect->x = MAX(0, x);
        rect->y = MAX(0, y);
        rect->w = content_size.width;
        rect->h = content_size.height;
        return;
    } else if (render_fit == SC_RENDER_FIT_STRETCHED) {
        rect->x = 0;
        rect->y = 0;
        rect->w = video_area.width;
        rect->h = video_area.height;
        return;
    }

    assert(is_icon || render_fit == SC_RENDER_FIT_LETTERBOX);

    if (is_optimal_size(video_area, content_size)) {
        rect->x = 0;
        rect->y = 0;
        rect->w = video_area.width;
        rect->h = video_area.height;
        return;
    }

    bool keep_width = (uint32_t) content_size.width * video_area.height
                    > (uint32_t) content_size.height * video_area.width;
    if (keep_width) {
        rect->x = 0;
        rect->w = video_area.width;
        rect->h = (float) video_area.width * content_size.height
                                           / content_size.width;
        rect->y = (video_area.height - rect->h) / 2.f;
    } else {
        rect->y = 0;
        rect->h = video_area.height;
        rect->w = (float) video_area.height * content_size.width
                                            / content_size.height;
        rect->x = (video_area.width - rect->w) / 2.f;
    }
}

static void
sc_screen_update_content_rect(struct sc_screen *screen) {
    bool is_icon = !screen->video || screen->disconnected;

    struct sc_size window_size = sc_sdl_get_window_size(screen->window);
    compute_content_rect(window_size, screen->content_size, is_icon,
                         screen->render_fit, &screen->rect);
}

// Handle sidebar button click — runs ADB commands for device control
static void
sc_screen_sidebar_click(struct sc_screen *screen, int btn) {
    if (btn < 0) return;

    switch (btn) {
        case SC_SIDEBAR_BTN_SCREENSHOT: {
            // Create Prints folder and save screenshot
#ifdef _WIN32
            system("if not exist Prints mkdir Prints");
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                "adb exec-out screencap -p > Prints\\print_%llu.png",
                (unsigned long long) SDL_GetTicks());
            system(cmd);
#else
            system("mkdir -p Prints");
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                "adb exec-out screencap -p > Prints/print_%llu.png",
                (unsigned long long) SDL_GetTicks());
            system(cmd);
#endif
            break;
        }
        case SC_SIDEBAR_BTN_AUDIO:
            // Toggle mute on Android side
            sc_sidebar_toggle_audio(&screen->sidebar);
            system("adb shell input keyevent 164");
            break;
        case SC_SIDEBAR_BTN_VOL_DOWN:
            system("adb shell input keyevent 25");
            break;
        case SC_SIDEBAR_BTN_VOL_UP:
            system("adb shell input keyevent 24");
            break;
        case SC_SIDEBAR_BTN_BACK:
            system("adb shell input keyevent 4");
            break;
        case SC_SIDEBAR_BTN_HOME:
            system("adb shell input keyevent 3");
            break;
        case SC_SIDEBAR_BTN_APP_SWITCH:
            system("adb shell input keyevent 187");
            break;
        default:
            break;
    }
}

static void
sc_screen_render(struct sc_screen *screen, bool update_content_rect) {
    assert(screen->window_shown);

    if (update_content_rect) {
        sc_screen_update_content_rect(screen);
    }

    SDL_Renderer *renderer = screen->renderer;
    struct sc_screen_bg_color bg = screen->bg;
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 0);
    sc_sdl_render_clear(renderer);

    SDL_Texture *texture = screen->tex.texture;
    if (!texture) {
        goto render_sidebar;
    }

    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale == 0) {
        LOGE("Cannot get scale value: %s", SDL_GetError());
        scale = 1;
    }

    SDL_FRect geometry = {
        .x = screen->rect.x * scale,
        .y = screen->rect.y * scale,
        .w = screen->rect.w * scale,
        .h = screen->rect.h * scale,
    };
    enum sc_orientation orientation = screen->orientation;

    bool ok = false;
    if (orientation == SC_ORIENTATION_0) {
        geometry.x = (int32_t) geometry.x;
        geometry.y = (int32_t) geometry.y;
        ok = SDL_RenderTexture(renderer, texture, NULL, &geometry);
    } else {
        unsigned cw_rotation = sc_orientation_get_rotation(orientation);
        double angle = 90 * cw_rotation;

        SDL_FRect *dstrect = NULL;
        SDL_FRect rect;
        if (sc_orientation_is_swap(orientation)) {
            rect.x = geometry.x + (geometry.w - geometry.h) / 2.f;
            rect.y = geometry.y + (geometry.h - geometry.w) / 2.f;
            rect.w = geometry.h;
            rect.h = geometry.w;
            dstrect = &rect;
        } else {
            dstrect = &geometry;
        }

        SDL_FlipMode flip = sc_orientation_is_mirror(orientation)
                              ? SDL_FLIP_HORIZONTAL : 0;

        dstrect->x = (int32_t) dstrect->x;
        dstrect->y = (int32_t) dstrect->y;
        ok = SDL_RenderTextureRotated(renderer, texture, NULL, dstrect, angle,
                                      NULL, flip);
    }

    if (!ok) {
        LOGE("Could not render texture: %s", SDL_GetError());
    }

render_sidebar:
    // Render sidebar on the right side
    if (screen->video && !screen->disconnected) {
        struct sc_size win = sc_sdl_get_window_size(screen->window);
        sc_sidebar_update_layout(&screen->sidebar,
                                 (int) win.width - SIDEBAR_WIDTH,
                                 (int) win.height);
        sc_sidebar_render(&screen->sidebar);
    }

    sc_sdl_render_present(renderer);
}

static void
sc_screen_request_resize_display(struct sc_screen *screen, uint16_t width,
                                 uint16_t height) {
    assert(screen->flex_display);
    assert(!screen->camera);
    if (sc_orientation_is_swap(screen->orientation)) {
        uint16_t tmp = width;
        width = height;
        height = tmp;
    }

    LOGV("resize_display(%" PRIu16 ", %" PRIu16 ")", width, height);
    sc_controller_resize_display(screen->controller, width, height);
}

static void
sc_screen_on_resize(struct sc_screen *screen, const SDL_WindowEvent *event) {
    if (!screen->window_shown) {
        return;
    }

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        sc_screen_render(screen, true);
    } else {
        assert(event->type == SDL_EVENT_WINDOW_RESIZED);
        if (screen->flex_display) {
            assert(!(event->data1 & ~0xFFFF));
            assert(!(event->data2 & ~0xFFFF));
            uint16_t width = event->data1;
            uint16_t height = event->data2;

            struct sc_resize_tracker *tracker = &screen->resize_tracker;
            if (tracker->time
                    && sc_tick_now() >= tracker->time + SC_TICK_FROM_MS(3000)) {
                tracker->time = 0;
            }
            if (tracker->time && tracker->size.width == width
                              && tracker->size.height == height) {
                LOGV("Ignore local resize: %" PRIu16 "x%" PRIu16,
                     width, height);
                tracker->time = 0;
            } else {
                sc_screen_request_resize_display(screen, width, height);
            }
        }
    }
}

#if defined(__APPLE__) || defined(_WIN32)
# define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
static bool
event_watcher(void *data, SDL_Event *event) {
    struct sc_screen *screen = data;
    assert(screen->video);

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
            || event->type == SDL_EVENT_WINDOW_RESIZED) {
        sc_screen_on_resize(screen, &event->window);
    }

    return true;
}
#endif

static bool
sc_screen_frame_sink_open(struct sc_frame_sink *sink,
                          const AVCodecContext *ctx,
                          const struct sc_stream_session *session) {
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);

    struct sc_screen *screen = DOWNCAST(sink);

    if (ctx->width <= 0 || ctx->width > 0xFFFF
            || ctx->height <= 0 || ctx->height > 0xFFFF) {
        LOGE("Invalid video size: %dx%d", ctx->width, ctx->height);
        return false;
    }

    screen->frame_size.width = session->video.width;
    screen->frame_size.height = session->video.height;
    screen->content_size = get_oriented_size(screen->frame_size,
                                             screen->orientation);

    screen->current_session = *session;

    bool ok = sc_push_event(SC_EVENT_OPEN_WINDOW);
    if (!ok) {
        return false;
    }

#ifndef NDEBUG
    screen->open = true;
#endif

    return true;
}

static void
sc_screen_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_screen *screen = DOWNCAST(sink);
    (void) screen;
#ifndef NDEBUG
    screen->open = false;
#endif
}

static bool
sc_screen_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_screen *screen = DOWNCAST(sink);
    assert(screen->video);

    sc_mutex_lock(&screen->mutex);
    bool previous_skipped = sc_frame_buffer_has_frame(&screen->fb);
    bool ok = sc_frame_buffer_push(&screen->fb, frame);
    screen->prevent_auto_resize = screen->current_session.video.client_resized;
    sc_mutex_unlock(&screen->mutex);
    if (!ok) {
        return false;
    }

    if (previous_skipped) {
        sc_fps_counter_add_skipped_frame(&screen->fps_counter);
    } else {
        bool ok = sc_push_event(SC_EVENT_NEW_FRAME);
        if (!ok) {
            return false;
        }
    }

    return true;
}

static bool
sc_screen_frame_sink_push_session(struct sc_frame_sink *sink,
                                  const struct sc_stream_session *session) {
    struct sc_screen *screen = DOWNCAST(sink);
    screen->current_session = *session;
    return true;
}

bool
sc_screen_init(struct sc_screen *screen,
               const struct sc_screen_params *params) {
    screen->controller = params->controller;

    screen->resize_pending = false;
    screen->window_shown = false;
    screen->paused = false;
    screen->resume_frame = NULL;
    screen->orientation = SC_ORIENTATION_0;
    screen->disconnected = false;
    screen->disconnect_started = false;

    screen->video = params->video;
    screen->camera = params->camera;
    screen->window_aspect_ratio_lock = params->window_aspect_ratio_lock;
    screen->render_fit = params->render_fit;
    screen->flex_display = params->flex_display;

    screen->bg.r = (params->background_color >> 16) & 0xFF;
    screen->bg.g = (params->background_color >> 8) & 0xFF;
    screen->bg.b = params->background_color & 0xFF;

    screen->req.x = params->window_x;
    screen->req.y = params->window_y;
    screen->req.width = params->window_width;
    screen->req.height = params->window_height;
    screen->req.fullscreen = params->fullscreen;
    screen->req.start_fps_counter = params->start_fps_counter;

    screen->prevent_auto_resize = false;

    screen->resize_tracker.time = 0;
    screen->resize_tracker.size.width = 0;
    screen->resize_tracker.size.height = 0;

    bool ok = sc_mutex_init(&screen->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_frame_buffer_init(&screen->fb);
    if (!ok) {
        goto error_destroy_mutex;
    }

    if (!sc_fps_counter_init(&screen->fps_counter)) {
        goto error_destroy_frame_buffer;
    }

    if (screen->video) {
        screen->orientation = params->orientation;
        if (screen->orientation != SC_ORIENTATION_0) {
            LOGI("Initial display orientation set to %s",
                 sc_orientation_get_name(screen->orientation));
        }
    }

    uint32_t window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (params->always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless) {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (params->video) {
        window_flags |= SDL_WINDOW_RESIZABLE;
    }

    const char *title = params->window_title;
    assert(title);

    int x = SDL_WINDOWPOS_UNDEFINED;
    int y = SDL_WINDOWPOS_UNDEFINED;
    int width = 256 + SIDEBAR_WIDTH;
    int height = 256;
    if (params->window_x != SC_WINDOW_POSITION_UNDEFINED) {
        x = params->window_x;
    }
    if (params->window_y != SC_WINDOW_POSITION_UNDEFINED) {
        y = params->window_y;
    }
    if (params->window_width) {
        width = params->window_width + SIDEBAR_WIDTH;
    }
    if (params->window_height) {
        height = params->window_height;
    }

    screen->window =
        sc_sdl_create_window(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        goto error_destroy_fps_counter;
    }

    screen->renderer = SDL_CreateRenderer(screen->window, NULL);
    if (!screen->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        goto error_destroy_window;
    }

    // Initialize sidebar
    sc_sidebar_init(&screen->sidebar, screen->renderer);

#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    screen->gl_context = NULL;

    const char *renderer_name = SDL_GetRendererName(screen->renderer);
    bool use_opengl = renderer_name && !strncmp(renderer_name, "opengl", 6);
    if (use_opengl) {
        bool ok = SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                      SDL_GL_CONTEXT_PROFILE_CORE);
        if (!ok) {
            LOGW("Could not set a GL Core Profile Context");
        }

        LOGD("Creating OpenGL Core Profile context");
        screen->gl_context = SDL_GL_CreateContext(screen->window);
        if (!screen->gl_context) {
            LOGE("Could not create OpenGL context: %s", SDL_GetError());
            goto error_destroy_renderer;
        }
    }
#endif

    bool mipmaps = params->video;
    ok = sc_texture_init(&screen->tex, screen->renderer, mipmaps);
    if (!ok) {
        goto error_destroy_renderer;
    }

    ok = SDL_StartTextInput(screen->window);
    if (!ok) {
        LOGE("Could not enable text input: %s", SDL_GetError());
        goto error_destroy_texture;
    }

    SDL_Surface *icon = sc_icon_load(SC_ICON_FILENAME_SCRCPY);
    if (icon) {
        if (!SDL_SetWindowIcon(screen->window, icon)) {
            LOGW("Could not set window icon: %s", SDL_GetError());
        }

        if (!params->video) {
            screen->content_size.width = icon->w;
            screen->content_size.height = icon->h;
            ok = sc_texture_set_from_surface(&screen->tex, icon);
            if (!ok) {
                LOGE("Could not set icon: %s", SDL_GetError());
            }
        }

        sc_icon_destroy(icon);
    } else {
        LOGE("Could not load icon");

        if (!params->video) {
            screen->content_size.width = 256;
            screen->content_size.height = 256;
        }
    }

    screen->frame = av_frame_alloc();
    if (!screen->frame) {
        LOG_OOM();
        goto error_destroy_texture;
    }

    struct sc_input_manager_params im_params = {
        .controller = params->controller,
        .fp = params->fp,
        .screen = screen,
        .kp = params->kp,
        .mp = params->mp,
        .gp = params->gp,
        .camera = params->camera,
        .mouse_bindings = params->mouse_bindings,
        .legacy_paste = params->legacy_paste,
        .clipboard_autosync = params->clipboard_autosync,
        .shortcut_mods = params->shortcut_mods,
    };

    sc_input_manager_init(&screen->im, &im_params);

    sc_mouse_capture_init(&screen->mc, screen->window, params->shortcut_mods);

#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (screen->video) {
        ok = SDL_AddEventWatch(event_watcher, screen);
        if (!ok) {
            LOGW("Could not add event watcher for continuous resizing: %s",
                 SDL_GetError());
        }
    }
#endif

    memset(&screen->current_session, 0, sizeof(screen->current_session));

    static const struct sc_frame_sink_ops ops = {
        .open = sc_screen_frame_sink_open,
        .close = sc_screen_frame_sink_close,
        .push = sc_screen_frame_sink_push,
        .push_session = sc_screen_frame_sink_push_session,
    };

    screen->frame_sink.ops = &ops;

#ifndef NDEBUG
    screen->open = false;
#endif

    if (!screen->video) {
        screen->window_shown = true;
        sc_sdl_show_window(screen->window);

        if (sc_screen_is_relative_mode(screen)) {
            sc_mouse_capture_set_active(&screen->mc, true);
        }
    }

    return true;

error_destroy_texture:
    sc_texture_destroy(&screen->tex);
error_destroy_renderer:
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    if (screen->gl_context) {
        SDL_GL_DestroyContext(screen->gl_context);
    }
#endif
    SDL_DestroyRenderer(screen->renderer);
error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_fps_counter:
    sc_fps_counter_destroy(&screen->fps_counter);
error_destroy_frame_buffer:
    sc_frame_buffer_destroy(&screen->fb);
error_destroy_mutex:
    sc_mutex_destroy(&screen->mutex);

    return false;
}

static void
sc_screen_show_initial_window(struct sc_screen *screen) {
    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.x : (int) SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.y : (int) SDL_WINDOWPOS_CENTERED;
    struct sc_point position = {
        .x = x,
        .y = y,
    };

    struct sc_size window_size =
        get_initial_optimal_size(screen->content_size, screen->req.width,
                                                       screen->req.height);

    // Add sidebar width to the window
    window_size.width += SIDEBAR_WIDTH;

    if (screen->flex_display
            && (window_size.width - SIDEBAR_WIDTH) == screen->content_size.width
            && window_size.height == screen->content_size.height) {
        sc_screen_track_resize(screen, window_size);
    }

    assert(is_windowed(screen));
    set_aspect_ratio(screen, screen->content_size);
    sc_sdl_set_window_size(screen->window, window_size);
    sc_sdl_set_window_position(screen->window, position);

    if (screen->req.fullscreen) {
        sc_screen_toggle_fullscreen(screen);
    }

    if (screen->req.start_fps_counter) {
        sc_fps_counter_start(&screen->fps_counter);
    }

    screen->window_shown = true;
    sc_sdl_show_window(screen->window);
    sc_screen_update_content_rect(screen);
}

void
sc_screen_hide_window(struct sc_screen *screen) {
    sc_sdl_hide_window(screen->window);
    screen->window_shown = false;
}

void
sc_screen_interrupt(struct sc_screen *screen) {
    sc_fps_counter_interrupt(&screen->fps_counter);
}

static void
sc_screen_interrupt_disconnect(struct sc_screen *screen) {
    if (screen->disconnect_started) {
        sc_disconnect_interrupt(&screen->disconnect);
    }
}

void
sc_screen_join(struct sc_screen *screen) {
    sc_fps_counter_join(&screen->fps_counter);
    if (screen->disconnect_started) {
        sc_disconnect_join(&screen->disconnect);
    }
}

void
sc_screen_destroy(struct sc_screen *screen) {
#ifndef NDEBUG
    assert(!screen->open);
#endif
    if (screen->disconnect_started) {
        sc_disconnect_destroy(&screen->disconnect);
    }
    sc_texture_destroy(&screen->tex);
    av_frame_free(&screen->frame);
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    SDL_GL_DestroyContext(screen->gl_context);
#endif
    SDL_DestroyRenderer(screen->renderer);
    SDL_DestroyWindow(screen->window);
    sc_fps_counter_destroy(&screen->fps_counter);
    sc_frame_buffer_destroy(&screen->fb);
    sc_mutex_destroy(&screen->mutex);

    SDL_Event event;
    int nevents = SDL_PeepEvents(&event, 1, SDL_GETEVENT,
                                 SC_EVENT_DISCONNECTED_ICON_LOADED,
                                 SC_EVENT_DISCONNECTED_ICON_LOADED);
    if (nevents == 1) {
        assert(event.type == SC_EVENT_DISCONNECTED_ICON_LOADED);
        SDL_Surface *dangling_icon = event.user.data1;
        sc_icon_destroy(dangling_icon);
    }
}

static void
resize_for_content(struct sc_screen *screen, struct sc_size old_content_size,
                   struct sc_size new_content_size) {
    assert(screen->video);

    struct sc_size target_size = new_content_size;
    if (!screen->flex_display) {
        struct sc_size window_size = sc_sdl_get_window_size(screen->window);
        // Subtract sidebar from window_size for calculation
        uint32_t video_w = (window_size.width > SIDEBAR_WIDTH)
                         ? window_size.width - SIDEBAR_WIDTH : window_size.width;
        target_size.width = (uint32_t) video_w * target_size.width
                          / old_content_size.width;
        target_size.height = (uint32_t) window_size.height * target_size.height
                           / old_content_size.height;
    }
    target_size = get_optimal_size(target_size, new_content_size, true);
    // Add sidebar back
    target_size.width += SIDEBAR_WIDTH;
    assert(is_windowed(screen));
    set_aspect_ratio(screen, new_content_size);
    sc_sdl_set_window_size(screen->window, target_size);
}

static void
set_content_size(struct sc_screen *screen, struct sc_size new_content_size,
                 bool resize) {
    assert(screen->video);

    if (resize) {
        if (is_windowed(screen)) {
            resize_for_content(screen, screen->content_size, new_content_size);
        } else if (screen->flex_display) {
            struct sc_size size = sc_sdl_get_window_size(screen->window);
            sc_screen_request_resize_display(screen, size.width, size.height);
        } else if (!screen->resize_pending) {
            screen->windowed_content_size = screen->content_size;
            screen->resize_pending = true;
        }
    }

    screen->content_size = new_content_size;
}

static void
apply_pending_resize(struct sc_screen *screen) {
    assert(screen->video);

    assert(is_windowed(screen));
    if (screen->resize_pending) {
        resize_for_content(screen, screen->windowed_content_size,
                                   screen->content_size);
        screen->resize_pending = false;
    }
}

void
sc_screen_set_orientation(struct sc_screen *screen,
                          enum sc_orientation orientation) {
    assert(screen->video);

    if (orientation == screen->orientation) {
        return;
    }

    struct sc_size new_content_size =
        get_oriented_size(screen->frame_size, orientation);

    set_content_size(screen, new_content_size, true);

    screen->orientation = orientation;
    LOGI("Display orientation set to %s", sc_orientation_get_name(orientation));

    sc_screen_render(screen, true);
}

static bool
sc_screen_apply_frame(struct sc_screen *screen, bool can_resize) {
    assert(screen->video);
    assert(screen->window_shown);

    sc_fps_counter_add_rendered_frame(&screen->fps_counter);

    AVFrame *frame = screen->frame;
    struct sc_size new_frame_size = {frame->width, frame->height};

    if (screen->frame_size.width != new_frame_size.width
            || screen->frame_size.height != new_frame_size.height) {

        screen->frame_size = new_frame_size;

        struct sc_size new_content_size =
            get_oriented_size(new_frame_size, screen->orientation);

        if (screen->flex_display) {
            sc_screen_track_resize(screen, new_content_size);
        }

        set_content_size(screen, new_content_size, can_resize);
        sc_screen_update_content_rect(screen);
    }

    bool ok = sc_texture_set_from_frame(&screen->tex, frame);
    if (!ok) {
        return false;
    }

    sc_screen_render(screen, false);
    return true;
}

static bool
sc_screen_update_frame(struct sc_screen *screen) {
    assert(screen->video);

    if (screen->paused) {
        if (!screen->resume_frame) {
            screen->resume_frame = av_frame_alloc();
            if (!screen->resume_frame) {
                LOG_OOM();
                return false;
            }
        } else {
            av_frame_unref(screen->resume_frame);
        }
        sc_mutex_lock(&screen->mutex);
        sc_frame_buffer_consume(&screen->fb, screen->resume_frame);
        sc_mutex_unlock(&screen->mutex);
        return true;
    }

    av_frame_unref(screen->frame);
    sc_mutex_lock(&screen->mutex);
    sc_frame_buffer_consume(&screen->fb, screen->frame);
    bool can_resize = !screen->prevent_auto_resize;
    sc_mutex_unlock(&screen->mutex);
    return sc_screen_apply_frame(screen, can_resize);
}

void
sc_screen_set_paused(struct sc_screen *screen, bool paused) {
    assert(screen->video);

    if (!paused && !screen->paused) {
        return;
    }

    if (screen->paused && screen->resume_frame) {
        av_frame_free(&screen->frame);
        screen->frame = screen->resume_frame;
        screen->resume_frame = NULL;
        bool ok = sc_screen_apply_frame(screen, true);
        if (!ok) {
            LOGE("Resume frame update failed");
        }
    }

    if (!paused) {
        LOGI("Display screen unpaused");
    } else if (!screen->paused) {
        LOGI("Display screen paused");
    } else {
        LOGI("Display screen re-paused");
    }

    screen->paused = paused;
}

void
sc_screen_toggle_fullscreen(struct sc_screen *screen) {
    assert(screen->video);

    bool req_fullscreen =
        !(SDL_GetWindowFlags(screen->window) & SDL_WINDOW_FULLSCREEN);

    bool ok = SDL_SetWindowFullscreen(screen->window, req_fullscreen);
    if (!ok) {
        LOGW("Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    LOGD("Requested %s mode", req_fullscreen ? "fullscreen" : "windowed");
}

void
sc_screen_resize_to_fit(struct sc_screen *screen) {
    assert(screen->video);

    if (!is_windowed(screen)) {
        return;
    }

    if (screen->render_fit == SC_RENDER_FIT_STRETCHED) {
        return;
    }

    struct sc_size window_size = sc_sdl_get_window_size(screen->window);

    if (screen->render_fit == SC_RENDER_FIT_UNSCALED) {
        struct sc_size content_size = screen->content_size;
        struct sc_size new_size = {
            .width = content_size.width + SIDEBAR_WIDTH,
            .height = content_size.height,
        };
        set_aspect_ratio(screen, content_size);
        sc_sdl_set_window_size(screen->window, new_size);

        int32_t x_offset = 0;
        if (content_size.width < window_size.width) {
            x_offset = (window_size.width - content_size.width) / 2;
        }
        int32_t y_offset = 0;
        if (content_size.height < window_size.height) {
            y_offset = (window_size.height - content_size.height) / 2;
        }
        assert(x_offset >= 0 && y_offset >= 0);
        if (x_offset || y_offset) {
            struct sc_point pos = sc_sdl_get_window_position(screen->window);
            pos.x += x_offset;
            pos.y += y_offset;
            sc_sdl_set_window_position(screen->window, pos);
        }

        LOGD("Resized to content size: %ux%u", content_size.width,
                                               content_size.height);
        return;
    }

    assert(screen->render_fit == SC_RENDER_FIT_LETTERBOX);

    struct sc_point point = sc_sdl_get_window_position(screen->window);

    // For optimal size calculation, use video area only
    struct sc_size video_area = {
        .width = (window_size.width > SIDEBAR_WIDTH)
               ? window_size.width - SIDEBAR_WIDTH : window_size.width,
        .height = window_size.height,
    };
    struct sc_size optimal_size =
        get_optimal_size(video_area, screen->content_size, false);
    optimal_size.width += SIDEBAR_WIDTH;

    assert(optimal_size.width <= window_size.width);
    assert(optimal_size.height <= window_size.height);

    struct sc_point new_position = {
        .x = point.x + (window_size.width - optimal_size.width) / 2,
        .y = point.y + (window_size.height - optimal_size.height) / 2,
    };

    set_aspect_ratio(screen, screen->content_size);
    sc_sdl_set_window_size(screen->window, optimal_size);
    sc_sdl_set_window_position(screen->window, new_position);
    LOGD("Resized to optimal size: %ux%u", optimal_size.width,
                                           optimal_size.height);
}

void
sc_screen_resize_to_pixel_perfect(struct sc_screen *screen) {
    assert(screen->video);

    if (!is_windowed(screen)) {
        return;
    }

    struct sc_size content_size = screen->content_size;
    struct sc_size new_size = {
        .width = content_size.width + SIDEBAR_WIDTH,
        .height = content_size.height,
    };
    set_aspect_ratio(screen, content_size);
    sc_sdl_set_window_size(screen->window, new_size);
    LOGD("Resized to pixel-perfect: %ux%u", content_size.width,
                                            content_size.height);
}

static void
sc_disconnect_on_icon_loaded(struct sc_disconnect *d, SDL_Surface *icon,
                             void *userdata) {
    (void) d;
    (void) userdata;

    bool ok = sc_push_event_with_data(SC_EVENT_DISCONNECTED_ICON_LOADED, icon);
    if (!ok) {
        sc_icon_destroy(icon);
    }
}

static void
sc_disconnect_on_timeout(struct sc_disconnect *d, void *userdata) {
    (void) d;
    (void) userdata;

    bool ok = sc_push_event(SC_EVENT_DISCONNECTED_TIMEOUT);
    (void) ok;
}

void
sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    switch (event->type) {
        case SC_EVENT_OPEN_WINDOW:
            sc_screen_show_initial_window(screen);

            if (sc_screen_is_relative_mode(screen)) {
                sc_mouse_capture_set_active(&screen->mc, true);
            }

            sc_screen_render(screen, false);
            return;
        case SC_EVENT_NEW_FRAME: {
            bool ok = sc_screen_update_frame(screen);
            if (!ok) {
                LOGE("Frame update failed\n");
            }
            return;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
            sc_screen_render(screen, true);
            return;
#ifndef CONTINUOUS_RESIZING_WORKAROUND
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            sc_screen_on_resize(screen, &event->window);
            return;
#endif
        case SDL_EVENT_WINDOW_RESTORED:
            if (screen->video && is_windowed(screen)) {
                apply_pending_resize(screen);
                sc_screen_render(screen, true);
            }
            return;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            LOGD("Switched to fullscreen mode");
            assert(screen->video);
            return;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            LOGD("Switched to windowed mode");
            assert(screen->video);
            if (is_windowed(screen)) {
                apply_pending_resize(screen);
                sc_screen_render(screen, true);
            }
            return;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            // Check if click is in sidebar area
            struct sc_size win = sc_sdl_get_window_size(screen->window);
            int sidebar_x = (int) win.width - SIDEBAR_WIDTH;
            if (event->button.x >= sidebar_x && screen->video
                    && !screen->disconnected) {
                int btn = sc_sidebar_get_button_at(&screen->sidebar,
                                                   event->button.x,
                                                   event->button.y);
                sc_screen_sidebar_click(screen, btn);
                return; // consumed
            }
            break;
        }
        case SC_EVENT_DEVICE_DISCONNECTED:
            assert(!screen->disconnected);
            screen->disconnected = true;
            if (!screen->window_shown) {
                return;
            }

            sc_input_manager_handle_event(&screen->im, event);

            sc_texture_reset(&screen->tex);
            sc_screen_render(screen, true);

            sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(2);
            static const struct sc_disconnect_callbacks cbs = {
                .on_icon_loaded = sc_disconnect_on_icon_loaded,
                .on_timeout = sc_disconnect_on_timeout,
            };
            bool ok =
                sc_disconnect_start(&screen->disconnect, deadline, &cbs, NULL);
            if (ok) {
                screen->disconnect_started = true;
            }

            return;
    }

    if (sc_screen_is_relative_mode(screen)
            && sc_mouse_capture_handle_event(&screen->mc, event)) {
        return;
    }

    sc_input_manager_handle_event(&screen->im, event);
}

void
sc_screen_handle_disconnection(struct sc_screen *screen) {
    if (!screen->window_shown) {
        return;
    }

    if (!screen->disconnect_started) {
        return;
    }

    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_WINDOW_EXPOSED:
                sc_screen_render(screen, true);
                break;
            case SC_EVENT_DISCONNECTED_ICON_LOADED: {
                SDL_Surface *icon_disconnected = event.user.data1;
                assert(icon_disconnected);

                bool ok = sc_texture_set_from_surface(&screen->tex,
                                                      icon_disconnected);
                if (ok) {
                    screen->content_size.width = icon_disconnected->w;
                    screen->content_size.height = icon_disconnected->h;
                    sc_screen_render(screen, true);
                } else {
                    LOGE("Could not set disconnected icon");
                }

                sc_icon_destroy(icon_disconnected);
                break;
            }
            case SC_EVENT_DISCONNECTED_TIMEOUT:
                LOGD("Closing after device disconnection");
                return;
            case SDL_EVENT_QUIT:
                LOGD("User requested to quit");
                sc_screen_interrupt_disconnect(screen);
                return;
            default:
                sc_input_manager_handle_event(&screen->im, &event);
        }
    }
}

struct sc_point
sc_screen_convert_window_to_frame_coords(struct sc_screen *screen,
                                         int32_t x, int32_t y) {
    assert(screen->video);

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    assert(screen->rect.w && screen->rect.h);

    x = (int64_t) (x - screen->rect.x) * w / screen->rect.w;
    y = (int64_t) (y - screen->rect.y) * h / screen->rect.h;

    struct sc_point result;
    switch (orientation) {
        case SC_ORIENTATION_0:
            result.x = x;
            result.y = y;
            break;
        case SC_ORIENTATION_90:
            result.x = y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_180:
            result.x = w - x;
            result.y = h - y;
            break;
        case SC_ORIENTATION_270:
            result.x = h - y;
            result.y = x;
            break;
        case SC_ORIENTATION_FLIP_0:
            result.x = w - x;
            result.y = y;
            break;
        case SC_ORIENTATION_FLIP_90:
            result.x = h - y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_FLIP_180:
            result.x = x;
            result.y = h - y;
            break;
        default:
            assert(orientation == SC_ORIENTATION_FLIP_270);
            result.x = y;
            result.y = x;
            break;
    }

    return result;
}
