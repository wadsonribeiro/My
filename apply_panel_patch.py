#!/usr/bin/env python3
"""
apply_panel_patch.py
====================
Run this script inside the root of your cloned scrcpy repository fork.
It will:
  1. Copy the new panel source files into app/src/panel/
  2. Patch app/src/screen.h to add the new struct members
  3. Patch app/src/screen.c to wire up the panel
  4. Patch app/src/input_manager.c to pass panel events first
  5. Patch meson.build to compile the new files

Usage:
    cd /path/to/your/scrcpy-fork
    python3 apply_panel_patch.py

The script is idempotent — it can be re-run safely.
"""

import sys, os, re, shutil, textwrap

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def read(path):
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()

def write(path, content):
    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else '.', exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f"  WRITE  {path}")

def patch_once(content, marker, insertion, after=True):
    """Insert `insertion` once, right after (or before) the first `marker`."""
    if insertion.strip() in content:
        return content  # already applied
    idx = content.find(marker)
    if idx == -1:
        raise ValueError(f"Marker not found: {marker!r}")
    if after:
        idx += len(marker)
    return content[:idx] + insertion + content[idx:]

def replace_once(content, old, new):
    if new.strip() in content:
        return content  # already applied
    if old not in content:
        raise ValueError(f"Target not found for replace: {old!r}")
    return content.replace(old, new, 1)

# ─────────────────────────────────────────────────────────────────────────────
# 1. Copy panel source files
# ─────────────────────────────────────────────────────────────────────────────

PANEL_SRC = os.path.join(SCRIPT_DIR, 'app', 'src', 'panel')
PANEL_DST = os.path.join('app', 'src', 'panel')

def copy_panel_files():
    os.makedirs(PANEL_DST, exist_ok=True)
    for fname in ['device_config.h', 'device_config.c',
                   'side_panel.h',   'side_panel.c',
                   'settings_dialog.h', 'settings_dialog.c']:
        src = os.path.join(PANEL_SRC, fname)
        dst = os.path.join(PANEL_DST, fname)
        if not os.path.exists(src):
            print(f"  SKIP   {fname} (source not found in {PANEL_SRC})")
            continue
        # Skip if src and dst resolve to the same file (script is inside the repo)
        src_abs = os.path.abspath(src)
        dst_abs = os.path.abspath(dst)
        if src_abs == dst_abs:
            print(f"  SKIP   {fname} (already in place)")
            continue
        shutil.copy2(src, dst)
        print(f"  COPY   {dst}")

# ─────────────────────────────────────────────────────────────────────────────
# 2. Patch screen.h
# ─────────────────────────────────────────────────────────────────────────────

SCREEN_H_INCLUDES = '''\
#include "panel/side_panel.h"
#include "panel/settings_dialog.h"
#include "panel/device_config.h"
'''

SCREEN_H_STRUCT_MEMBERS = '''\
\n    /* ---- Side panel & settings ---- */
    sc_side_panel       side_panel;
    sc_settings_dialog  settings_dialog;
    sc_device_config    device_cfg;
    bool                restart_requested;
    char                restart_cmd[512];
'''

def patch_screen_h():
    path = os.path.join('app', 'src', 'screen.h')
    c = read(path)
    # Includes
    c = patch_once(c, '#include "input_manager.h"', '\n' + SCREEN_H_INCLUDES)
    # Struct members — insert before the closing }; of struct sc_screen
    # Find last member we know (struct sc_input_manager im;)
    c = patch_once(c, 'struct sc_input_manager im;', SCREEN_H_STRUCT_MEMBERS)
    write(path, c)

# ─────────────────────────────────────────────────────────────────────────────
# 3. Patch screen.c
# ─────────────────────────────────────────────────────────────────────────────

SCREEN_C_INCLUDES = '''\
#include "panel/side_panel.h"
#include "panel/settings_dialog.h"
#include "panel/device_config.h"
'''

PANEL_CALLBACKS = r'''
/* =========================================================================
 * Side-panel callbacks  (injected by apply_panel_patch.py)
 * ========================================================================= */

#ifdef _WIN32
# include <windows.h>
# include <shlwapi.h>
#else
# include <libgen.h>
# include <time.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

static void
panel_screenshot(void *userdata) {
    struct sc_screen *screen = userdata;
    char prints_dir[512];

#ifdef _WIN32
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    PathRemoveFileSpecA(exe);
    snprintf(prints_dir, sizeof(prints_dir), "%s\\Prints", exe);
    CreateDirectoryA(prints_dir, NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
             st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "adb -s %s exec-out screencap -p > \"%s\\print_%s.png\"",
        screen->device_cfg.device_id, prints_dir, ts);
    system(cmd);
#else
    char exe_buf[4096]; ssize_t n;
    n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf)-1);
    if (n > 0) exe_buf[n] = '\0'; else strcpy(exe_buf, ".");
    snprintf(prints_dir, sizeof(prints_dir), "%s/Prints", dirname(exe_buf));
    mkdir(prints_dir, 0755);
    struct timespec tp; clock_gettime(CLOCK_REALTIME, &tp);
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
        "adb -s %s exec-out screencap -p > \"%s/print_%ld.png\"",
        screen->device_cfg.device_id, prints_dir, (long)tp.tv_sec);
    system(cmd);
#endif
    LOGI("Screenshot saved to: %s", prints_dir);
}

static void
panel_audio_toggle(void *userdata, bool *audio_on) {
    struct sc_screen *screen = userdata;
    screen->device_cfg.audio = *audio_on;
    sc_device_config_build_cmd(&screen->device_cfg,
                               screen->restart_cmd,
                               sizeof(screen->restart_cmd));
    screen->restart_requested = true;
}

static void
panel_volume(void *userdata, int delta) {
    struct sc_screen *screen = userdata;
    int keycode = (delta > 0) ? 24 : 25;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "adb -s %s shell input keyevent %d",
             screen->device_cfg.device_id, keycode);
    system(cmd);
}

static void
panel_nav(void *userdata, sp_button_id btn) {
    struct sc_screen *screen = userdata;
    int key = 0;
    switch (btn) {
        case SP_BTN_BACK:    key = 4;   break;
        case SP_BTN_HOME:    key = 3;   break;
        case SP_BTN_RECENTS: key = 187; break;
        default: return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "adb -s %s shell input keyevent %d",
             screen->device_cfg.device_id, key);
    system(cmd);
}

static void
panel_open_settings(void *userdata) {
    struct sc_screen *screen = userdata;
    sc_settings_dialog_show(&screen->settings_dialog);
}

static void
settings_save_restart(void *userdata, const sc_device_config *new_cfg) {
    struct sc_screen *screen = userdata;
    sc_device_config_build_cmd(new_cfg,
                               screen->restart_cmd,
                               sizeof(screen->restart_cmd));
    screen->restart_requested = true;
    LOGI("Restart requested with: %s", screen->restart_cmd);
}

/* ---- Panel initialisation called at the end of sc_screen_init ---- */
static bool
sc_screen_panel_init(struct sc_screen *screen, const char *device_serial) {
    sc_device_config_load(&screen->device_cfg, device_serial);

    TTF_Font *font = NULL;
    if (TTF_WasInit() || TTF_Init() == 0) {
        const char *paths[] = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/SFNSText.ttf",
#else
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
            NULL
        };
        for (int i = 0; paths[i] && !font; i++)
            font = TTF_OpenFont(paths[i], 13);
    }

    sc_side_panel_params sp = {
        .renderer        = screen->display.renderer,
        .font            = font,
        .device_cfg      = &screen->device_cfg,
        .on_screenshot   = panel_screenshot,
        .on_audio_toggle = panel_audio_toggle,
        .on_volume       = panel_volume,
        .on_nav          = panel_nav,
        .on_settings     = panel_open_settings,
        .userdata        = screen,
    };
    if (!sc_side_panel_init(&screen->side_panel, &sp)) return false;

    sc_settings_dialog_params sd = {
        .renderer        = screen->display.renderer,
        .font            = font,
        .font_bold       = font,
        .device_cfg      = &screen->device_cfg,
        .on_save_restart = settings_save_restart,
        .userdata        = screen,
    };
    if (!sc_settings_dialog_init(&screen->settings_dialog, &sd)) return false;

    screen->restart_requested = false;
    screen->restart_cmd[0]    = '\0';
    return true;
}
/* =========================================================================
 * END side-panel injection
 * ========================================================================= */
'''

RENDER_PANEL_HOOK = '''\
\n    /* Panel overlay */
    {
        int pw, ph;
        SDL_GetWindowSize(screen->window, &pw, &ph);
        sc_side_panel_update(&screen->side_panel, pw, ph);
        sc_side_panel_render(&screen->side_panel);
        if (screen->settings_dialog.visible)
            sc_settings_dialog_render(&screen->settings_dialog, pw, ph);
    }
'''

def patch_screen_c():
    path = os.path.join('app', 'src', 'screen.c')
    c = read(path)

    # Includes
    c = patch_once(c, '#include "version.h"', '\n' + SCREEN_C_INCLUDES)

    # Inject callback block after the first #define in the file
    c = patch_once(c, '#define SC_WINDOW_TITLE', PANEL_CALLBACKS, after=False)

    # Hook render: find SDL_RenderPresent call and add panel overlay right after
    c = patch_once(c, 'SDL_RenderPresent(display->renderer);', RENDER_PANEL_HOOK)

    # Hook sc_screen_init: call sc_screen_panel_init after sc_input_manager_init
    c = patch_once(c, 'sc_input_manager_init(&screen->im, &im_params);',
                   '''\n\n    /* Panel init */
    if (!sc_screen_panel_init(screen,
            params->serial ? params->serial : "")) {
        LOGE("Panel init failed (non-fatal)");
    }
''')

    write(path, c)

# ─────────────────────────────────────────────────────────────────────────────
# 4. Patch input_manager.c — pass events to panel first
# ─────────────────────────────────────────────────────────────────────────────

def patch_input_manager_c():
    path = os.path.join('app', 'src', 'input_manager.c')
    c = read(path)

    # We need to include screen.h (already included) so we can access panel
    # Find sc_input_manager_process_event and divert to panel first
    hook = '''\

    /* Side-panel intercept */
    {
        int pw, ph;
        SDL_GetWindowSize(SDL_GetWindowFromID(event->window.windowID
                          ? event->window.windowID : 1), &pw, &ph);
        if (im->screen) {
            struct sc_screen *scr = im->screen;
            if (scr->settings_dialog.visible) {
                if (sc_settings_dialog_handle_event(
                        &scr->settings_dialog, event, pw, ph))
                    return;
            } else if (sc_side_panel_handle_event(&scr->side_panel, event)) {
                return;
            }
        }
    }
'''
    # Insert at beginning of sc_input_manager_handle_event body
    c = patch_once(c, 'sc_input_manager_handle_event(', hook,
                   after=False)

    write(path, c)

# ─────────────────────────────────────────────────────────────────────────────
# 5. Patch meson.build — add new source files
# ─────────────────────────────────────────────────────────────────────────────

NEW_SOURCES = """\
    'app/src/panel/device_config.c',
    'app/src/panel/side_panel.c',
    'app/src/panel/settings_dialog.c',
"""

def patch_meson_build():
    path = 'meson.build'
    c = read(path)
    # Find the sources list and add our files before the closing bracket
    # Look for 'app/src/screen.c' as anchor
    anchor = "'app/src/screen.c',"
    if NEW_SOURCES.strip() in c:
        print(f"  SKIP   meson.build (already patched)")
        return
    c = patch_once(c, anchor, '\n' + NEW_SOURCES)

    # Add SDL2_ttf to dependencies if not present
    if 'sdl2_ttf' not in c and 'SDL2_ttf' not in c:
        # Try to add after sdl2 dep declaration
        c = patch_once(c, "dependency('sdl2'",
                       "\nsdl2_ttf_dep = dependency('SDL2_ttf', required: false)", after=False)
        c = patch_once(c, "sdl2_dep,", "\n    sdl2_ttf_dep,")

    write(path, c)

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    if not os.path.exists('meson.build'):
        print("ERROR: Run this script from the root of your scrcpy repository.")
        sys.exit(1)

    print("=== Applying side-panel patch to scrcpy ===\n")

    print("[1/5] Copying panel source files...")
    copy_panel_files()

    print("\n[2/5] Patching app/src/screen.h...")
    patch_screen_h()

    print("\n[3/5] Patching app/src/screen.c...")
    patch_screen_c()

    print("\n[4/5] Patching app/src/input_manager.c...")
    patch_input_manager_c()

    print("\n[5/5] Patching meson.build...")
    patch_meson_build()

    print("""
=== Patch applied successfully! ===

Next steps to build on Linux/macOS:
    meson setup build --buildtype=release
    cd build && ninja

Next steps to cross-compile for Windows (from Linux):
    meson setup build-win --cross-file cross_win64.txt --buildtype=release
    cd build-win && ninja

Or push to GitHub and let the Actions workflow build automatically.
""")

if __name__ == '__main__':
    main()
