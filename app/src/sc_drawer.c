/**
 * sc_drawer.c — Implementação da gaveta retrátil para scrcpy
 *
 * Renderização: SDL2 (sem dependência extra de UI)
 * Config: JSON mínimo (sem cJSON — parser próprio leve)
 * ADB:    subprocess via SDL_system ou popen()
 *
 * Integração:
 *   1. Copie sc_drawer.h e sc_drawer.c para app/src/
 *   2. Adicione 'sc_drawer.c' em app/meson.build (sources)
 *   3. Em screen.c: inclua sc_drawer.h, adicione sc_drawer no struct sc_screen,
 *      inicialize em sc_screen_init(), chame update+render em sc_screen_render()
 *      e passe eventos em sc_screen_handle_event()
 */

#include "sc_drawer.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  define POPEN  _popen
#  define PCLOSE _pclose
#  define PATH_SEP "\\"
#else
#  include <sys/stat.h>
#  define POPEN  popen
#  define PCLOSE pclose
#  define PATH_SEP "/"
#endif

/* ─────────────────────────────────────────────────────────────────────
 * CONSTANTES DE LAYOUT
 * ───────────────────────────────────────────────────────────────────── */
#define DRAWER_W_PX        58    /* largura do painel de botões        */
#define TAB_W_PX           20    /* largura da aba toggle              */
#define BTN_SIZE           40    /* botão quadrado (px)                */
#define BTN_RADIUS         20    /* raio para rounded rect             */
#define BTN_GAP            8     /* espaço vertical entre botões       */
#define DRAWER_PAD_X       9     /* padding horizontal interno         */
#define DRAWER_PAD_TOP     14    /* padding topo                       */
#define ANIM_SPEED         0.012f /* fração por ms (0.0–1.0)          */

/* Cores (RGBA) */
#define COL_BG_DRAWER   0x0A, 0x08, 0x1A, 0xE8   /* fundo da gaveta        */
#define COL_BG_TAB      0x50, 0x3C, 0xB4, 0xCC   /* aba toggle             */
#define COL_BTN_NORMAL  0x28, 0x1E, 0x50, 0xCC   /* botão normal           */
#define COL_BTN_HOVER   0x64, 0x50, 0xC8, 0xDD   /* botão hover            */
#define COL_BTN_PRESS   0x40, 0x28, 0x90, 0xFF   /* botão pressionado      */
#define COL_BTN_BORDER  0x78, 0x60, 0xD0, 0x66   /* borda dos botões       */
#define COL_TEXT        0xE0, 0xE0, 0xFF, 0xFF   /* texto principal        */
#define COL_TEXT_DIM    0x7C, 0x7C, 0xAA, 0xFF   /* texto secundário       */
#define COL_ACCENT      0xA7, 0x8B, 0xFA, 0xFF   /* roxo accent            */
#define COL_FLASH_OK    0x4A, 0xDE, 0x80, 0xFF   /* flash verde "OK"       */
#define COL_CFG_BG      0x0F, 0x0A, 0x22, 0xF8   /* fundo config dialog    */
#define COL_SEP         0x2A, 0x2A, 0x4A, 0xFF   /* separador              */

/* Keycodes ADB */
#define KEYCODE_BACK       4
#define KEYCODE_HOME       3
#define KEYCODE_APP_SWITCH 187
#define KEYCODE_VOL_UP     24
#define KEYCODE_VOL_DOWN   25

/* Índices FPS/RES/BPS */
static const int   FPS_OPTIONS[]  = {30, 60, 120, 240};
static const int   RES_OPTIONS[]  = {540, 720, 1080, 1440};
static const char *BPS_OPTIONS[]  = {"4M","8M","16M","32M"};
#define OPT_COUNT 4

/* ─────────────────────────────────────────────────────────────────────
 * HELPER: desenhar retângulo arredondado (SDL2 puro)
 * ───────────────────────────────────────────────────────────────────── */
static void draw_rounded_rect(SDL_Renderer *r,
                               int x, int y, int w, int h,
                               int radius,
                               Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);

    /* Centro */
    SDL_Rect center = {x + radius, y, w - 2*radius, h};
    SDL_RenderFillRect(r, &center);
    SDL_Rect left   = {x, y + radius, radius, h - 2*radius};
    SDL_RenderFillRect(r, &left);
    SDL_Rect right  = {x + w - radius, y + radius, radius, h - 2*radius};
    SDL_RenderFillRect(r, &right);

    /* Cantos (quadrante de círculo aproximado com linhas) */
    for (int dy = 0; dy < radius; dy++) {
        int dx = (int)(radius - sqrtf((float)(radius*radius - dy*dy)));
        SDL_RenderDrawLine(r,
            x + dx,           y + radius - dy - 1,
            x + w - dx - 1,   y + radius - dy - 1);
        SDL_RenderDrawLine(r,
            x + dx,           y + h - radius + dy,
            x + w - dx - 1,   y + h - radius + dy);
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * HELPER: texto simples via SDL2 pixel font (3x5)
 * Para texto real use SDL_ttf, mas este não precisa de deps externas.
 * ───────────────────────────────────────────────────────────────────── */

/* Labels dos botões (emojis como texto ASCII simplificado) */
static const char *BTN_LABELS[] = {
    "[SS]",  /* screenshot    */
    "[AU]",  /* audio         */
    "[V+]",  /* vol up        */
    "[V-]",  /* vol down      */
    "[ < ]", /* back          */
    "[ O ]", /* home          */
    "[ # ]", /* recents       */
    "[CF]",  /* settings      */
    "[ > ]", /* toggle (open) */
};

static const char *BTN_LABELS_EXP[] = {
    /* quando expandido */
    "[SS]", "[AU]", "[V+]", "[V-]", "[ < ]", "[ O ]", "[ # ]", "[CF]", "[ < ]",
};

/* Renderiza texto pixel ultra-simples usando SDL_RenderDrawPoint */
/* (Para integração real, substitua por SDL_ttf com uma fonte bitmap) */
static void draw_label(SDL_Renderer *r, const char *text, int cx, int cy,
                       Uint8 cr, Uint8 cg, Uint8 cb)
{
    /* Placeholder: apenas um ponto central para indicar existência do botão.
       Em produção, use SDL_ttf:
         SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
         SDL_Texture *tex  = SDL_CreateTextureFromSurface(r, surf);
         SDL_RenderCopy(r, tex, NULL, &dst);
       Ou renderize SVG icons via rsvg.
    */
    SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
    /* Desenha um pequeno "+" como placeholder */
    for (int i = -6; i <= 6; i++) {
        SDL_RenderDrawPoint(r, cx + i, cy);
        SDL_RenderDrawPoint(r, cx, cy + i);
    }
    /* Primeira letra do label como identificador */
    (void)text;
}

/* ─────────────────────────────────────────────────────────────────────
 * CONFIG JSON — parser mínimo (sem dependência externa)
 * ───────────────────────────────────────────────────────────────────── */

/* Extrai valor de string de um JSON key:value simples */
static bool json_get_str(const char *json, const char *key, char *out, size_t out_sz)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return false;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return false;
    while (*pos == ':' || *pos == ' ') pos++;
    if (*pos == '"') {
        pos++;
        size_t i = 0;
        while (*pos && *pos != '"' && i < out_sz - 1)
            out[i++] = *pos++;
        out[i] = '\0';
        return true;
    }
    /* number */
    size_t i = 0;
    while (*pos && *pos != ',' && *pos != '}' && *pos != '\n' && i < out_sz - 1)
        out[i++] = *pos++;
    out[i] = '\0';
    return true;
}

void sc_drawer_default_config(sc_device_config *cfg, const char *device_id)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->device_id, device_id, SC_DRAWER_MAX_ID_LEN - 1);
    cfg->max_fps    = 60;
    cfg->resolution = 1080;
    strncpy(cfg->bitrate, "8M", sizeof(cfg->bitrate) - 1);
    cfg->no_audio   = false;
}

bool sc_drawer_load_config(sc_device_config *cfg, const char *device_id)
{
    FILE *f = fopen(SC_DRAWER_CONFIG_FILE, "r");
    if (!f) {
        sc_drawer_default_config(cfg, device_id);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); sc_drawer_default_config(cfg, device_id); return false; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    /* Encontra o bloco do device_id */
    char key_str[SC_DRAWER_MAX_ID_LEN + 4];
    snprintf(key_str, sizeof(key_str), "\"%s\"", device_id);
    const char *block = strstr(buf, key_str);

    sc_drawer_default_config(cfg, device_id);

    if (block) {
        /* Avança até o '{' do bloco */
        block = strchr(block, '{');
        if (block) {
            char val[32];
            if (json_get_str(block, "max_fps", val, sizeof(val)))
                cfg->max_fps = atoi(val);
            if (json_get_str(block, "resolution", val, sizeof(val)))
                cfg->resolution = atoi(val);
            if (json_get_str(block, "bitrate", val, sizeof(val)))
                strncpy(cfg->bitrate, val, sizeof(cfg->bitrate) - 1);
        }
    }

    free(buf);
    return true;
}

bool sc_drawer_save_config(const sc_device_config *cfg)
{
    /* Lê o JSON existente */
    char *existing = NULL;
    long  exist_sz = 0;
    FILE *f = fopen(SC_DRAWER_CONFIG_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        exist_sz = ftell(f);
        rewind(f);
        existing = (char *)malloc(exist_sz + 1);
        if (existing) { fread(existing, 1, exist_sz, f); existing[exist_sz] = '\0'; }
        fclose(f);
    }

    /* Novo bloco para este device */
    char new_block[512];
    snprintf(new_block, sizeof(new_block),
        "  \"%s\": {\n"
        "    \"device_id\": \"%s\",\n"
        "    \"max_fps\": %d,\n"
        "    \"resolution\": %d,\n"
        "    \"bitrate\": \"%s\"\n"
        "  }",
        cfg->device_id, cfg->device_id,
        cfg->max_fps, cfg->resolution, cfg->bitrate);

    f = fopen(SC_DRAWER_CONFIG_FILE, "w");
    if (!f) { free(existing); return false; }

    if (!existing || exist_sz == 0) {
        /* Cria JSON do zero */
        fprintf(f, "{\n%s\n}\n", new_block);
    } else {
        /* Tenta substituir bloco existente ou adicionar */
        char search_key[SC_DRAWER_MAX_ID_LEN + 4];
        snprintf(search_key, sizeof(search_key), "\"%s\"", cfg->device_id);
        char *pos = strstr(existing, search_key);

        if (pos) {
            /* Substitui bloco existente */
            /* Encontra início do bloco (antes de "device_id") */
            char *brace_open = strchr(pos, '{');
            char *brace_close = NULL;
            if (brace_open) {
                int depth = 1; char *p = brace_open + 1;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    if (*p == '}') depth--;
                    p++;
                }
                brace_close = p;
            }
            if (brace_open && brace_close) {
                /* escreve tudo antes + novo bloco + tudo depois */
                fwrite(existing, 1, pos - existing, f);
                fputs(new_block, f);
                fputs(brace_close, f);
            } else {
                fputs(existing, f);
            }
        } else {
            /* Adiciona novo device antes do último '}' */
            char *last_brace = strrchr(existing, '}');
            if (last_brace) {
                fwrite(existing, 1, last_brace - existing, f);
                /* Verifica se precisa de vírgula */
                char *p = last_brace - 1;
                while (p > existing && (*p == ' ' || *p == '\n' || *p == '\r')) p--;
                if (*p != '{') fputs(",\n", f);
                fputs(new_block, f);
                fputs("\n}\n", f);
            } else {
                fprintf(f, "{\n%s\n}\n", new_block);
            }
        }
    }

    fclose(f);
    free(existing);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────
 * ADB ACTIONS
 * ───────────────────────────────────────────────────────────────────── */
static void run_adb(const char *device_id, const char *args)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "adb -s %s %s", device_id, args);
#ifdef _WIN32
    /* No Windows, executa sem janela */
    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    char full_cmd[600];
    snprintf(full_cmd, sizeof(full_cmd), "cmd /c %s", cmd);
    CreateProcessA(NULL, full_cmd, NULL, NULL, FALSE,
                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    system(cmd);
#endif
}

void sc_drawer_action_screenshot(sc_drawer *drawer)
{
    /* Cria pasta Prints se não existir */
#ifdef _WIN32
    CreateDirectoryA(SC_DRAWER_PRINTS_DIR, NULL);
#else
    mkdir(SC_DRAWER_PRINTS_DIR, 0755);
#endif

    /* Nome com timestamp */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char filename[256];
    strftime(filename, sizeof(filename),
             SC_DRAWER_PRINTS_DIR PATH_SEP "print_%Y%m%d_%H%M%S.png", tm);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "adb -s %s exec-out screencap -p > \"%s\"",
             drawer->cfg.device_id, filename);

#ifdef _WIN32
    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    char full_cmd[600];
    snprintf(full_cmd, sizeof(full_cmd), "cmd /c %s", cmd);
    CreateProcessA(NULL, full_cmd, NULL, NULL, FALSE,
                   CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    system(cmd);
#endif

    drawer->screenshot_flash_until = SDL_GetTicks() + 1500;
}

void sc_drawer_action_volume(sc_drawer *drawer, bool up)
{
    char args[64];
    snprintf(args, sizeof(args), "shell input keyevent %d",
             up ? KEYCODE_VOL_UP : KEYCODE_VOL_DOWN);
    run_adb(drawer->cfg.device_id, args);
}

void sc_drawer_action_nav(sc_drawer *drawer, int keycode)
{
    char args[64];
    snprintf(args, sizeof(args), "shell input keyevent %d", keycode);
    run_adb(drawer->cfg.device_id, args);
}

/* ─────────────────────────────────────────────────────────────────────
 * RESTART SCRCPY
 * ───────────────────────────────────────────────────────────────────── */
void sc_drawer_restart_scrcpy(sc_drawer *drawer)
{
    /* Envia evento SDL customizado para o loop principal reiniciar scrcpy */
    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));
    ev.type = SDL_USEREVENT;
    ev.user.code  = 0xDC01;  /* código "drawer config changed" */
    ev.user.data1 = (void *)&drawer->cfg;
    SDL_PushEvent(&ev);
}

/* ─────────────────────────────────────────────────────────────────────
 * CÁLCULO DE GEOMETRIA DOS BOTÕES
 * ───────────────────────────────────────────────────────────────────── */
static void recalc_button_rects(sc_drawer *d)
{
    int total_w = TAB_W_PX + DRAWER_W_PX;
    int base_x  = d->win_w - total_w;   /* x da gaveta completa */

    /* Aba toggle */
    int center_y = (d->win_h - BTN_SIZE) / 2;
    d->btn_rects[SC_DRAWER_BTN_TOGGLE] = (SDL_Rect){
        base_x, center_y, TAB_W_PX, BTN_SIZE
    };

    /* Botões internos — coluna vertical centrada */
    int bx = base_x + TAB_W_PX + DRAWER_PAD_X;
    int by = DRAWER_PAD_TOP;

    int order[] = {
        SC_DRAWER_BTN_SCREENSHOT,
        SC_DRAWER_BTN_AUDIO,
        SC_DRAWER_BTN_VOL_UP,
        SC_DRAWER_BTN_VOL_DOWN,
        SC_DRAWER_BTN_BACK,
        SC_DRAWER_BTN_HOME,
        SC_DRAWER_BTN_RECENTS,
        SC_DRAWER_BTN_SETTINGS,
    };

    /* Separa em grupos com gap extra */
    int extra_gaps[] = {0, 0, 4, 0, 4, 0, 0, 8}; /* gap extra ANTES deste botão */

    for (int i = 0; i < 8; i++) {
        by += extra_gaps[i];
        d->btn_rects[order[i]] = (SDL_Rect){bx, by, BTN_SIZE, BTN_SIZE};
        by += BTN_SIZE + BTN_GAP;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * INICIALIZAÇÃO / DESTRUIÇÃO
 * ───────────────────────────────────────────────────────────────────── */
bool sc_drawer_init(sc_drawer *drawer,
                    SDL_Window *window,
                    SDL_Renderer *renderer,
                    const char *device_id)
{
    memset(drawer, 0, sizeof(*drawer));
    drawer->window   = window;
    drawer->renderer = renderer;
    drawer->btn_hover     = -1;
    drawer->drawer_w      = DRAWER_W_PX;
    drawer->tab_w         = TAB_W_PX;
    drawer->expanded      = false;
    drawer->slide_t       = 0.0f;
    drawer->settings_open = false;

    /* Índices padrão nas listas de opções */
    drawer->settings_fps_sel = 1; /* 60 fps */
    drawer->settings_res_sel = 2; /* 1080   */
    drawer->settings_bps_sel = 1; /* 8M     */

    sc_drawer_load_config(&drawer->cfg, device_id);

    /* Sincroniza índices de seleção com cfg carregada */
    for (int i = 0; i < OPT_COUNT; i++) {
        if (FPS_OPTIONS[i] == drawer->cfg.max_fps) drawer->settings_fps_sel = i;
        if (RES_OPTIONS[i] == drawer->cfg.resolution) drawer->settings_res_sel = i;
        if (strcmp(BPS_OPTIONS[i], drawer->cfg.bitrate) == 0) drawer->settings_bps_sel = i;
    }

    SDL_GetWindowSize(window, &drawer->win_w, &drawer->win_h);
    recalc_button_rects(drawer);

    return true;
}

void sc_drawer_destroy(sc_drawer *drawer)
{
    (void)drawer; /* nada a liberar ainda */
}

void sc_drawer_resize(sc_drawer *drawer, int new_w, int new_h)
{
    drawer->win_w = new_w;
    drawer->win_h = new_h;
    recalc_button_rects(drawer);
}

/* ─────────────────────────────────────────────────────────────────────
 * RENDER
 * ───────────────────────────────────────────────────────────────────── */

/* Slide offset: 0 = só aba visível, DRAWER_W_PX = totalmente aberta */
static int slide_offset(sc_drawer *d)
{
    return (int)(d->slide_t * DRAWER_W_PX);
}

static void render_button(SDL_Renderer *r,
                           SDL_Rect rect,
                           bool hover, bool special,
                           const char *label)
{
    Uint8 bg_r, bg_g, bg_b, bg_a;
    if (hover)   { bg_r=0x64;bg_g=0x50;bg_b=0xC8;bg_a=0xDD; }
    else         { bg_r=0x28;bg_g=0x1E;bg_b=0x50;bg_a=0xCC; }

    if (special) { bg_b += 0x20; } /* settings = amarelado */

    draw_rounded_rect(r,
        rect.x, rect.y, rect.w, rect.h, 10,
        bg_r, bg_g, bg_b, bg_a);

    /* Borda */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0x78, 0x60, 0xD0, hover ? 0xCC : 0x66);
    SDL_Rect border = rect;
    SDL_RenderDrawRect(r, &border);

    /* Ícone placeholder */
    int cx = rect.x + rect.w / 2;
    int cy = rect.y + rect.h / 2;
    draw_label(r, label, cx, cy, 0xE0, 0xE0, 0xFF);
}

static void render_settings_dialog(sc_drawer *d)
{
    SDL_Renderer *r = d->renderer;
    int dw = 320, dh = 280;
    int dx = (d->win_w - dw) / 2;
    int dy = (d->win_h - dh) / 2;

    /* Fundo semitransparente sobre tudo */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0x00, 0x00, 0x00, 0xA0);
    SDL_Rect full = {0, 0, d->win_w, d->win_h};
    SDL_RenderFillRect(r, &full);

    /* Painel */
    draw_rounded_rect(r, dx, dy, dw, dh, 14,
                      0x0F, 0x0A, 0x22, 0xF8);
    SDL_SetRenderDrawColor(r, 0x4A, 0x4A, 0x8A, 0xFF);
    SDL_Rect panel = {dx, dy, dw, dh};
    SDL_RenderDrawRect(r, &panel);

    /* Título */
    /* Em produção: renderize texto com SDL_ttf aqui */
    /* Placeholder: linha horizontal de cor */
    SDL_SetRenderDrawColor(r, COL_ACCENT);
    SDL_RenderDrawLine(r, dx+14, dy+36, dx+dw-14, dy+36);

    /* Linhas de opção — FPS, RES, BPS */
    static const char *section_labels[] = {"FPS:", "RES:", "BPS:"};
    int *sel[] = {&d->settings_fps_sel, &d->settings_res_sel, &d->settings_bps_sel};

    /* Para cada seção, renderiza 4 botões de opção */
    for (int s = 0; s < 3; s++) {
        int row_y = dy + 50 + s * 64;
        /* Label da seção — placeholder linha */
        SDL_SetRenderDrawColor(r, COL_TEXT_DIM);
        SDL_RenderDrawLine(r, dx+14, row_y+6, dx+70, row_y+6);

        for (int i = 0; i < OPT_COUNT; i++) {
            int bx = dx + 80 + i * 56;
            bool selected = (*sel[s] == i);
            draw_rounded_rect(r, bx, row_y, 50, 28, 8,
                selected ? 0x7C : 0x28,
                selected ? 0x3A : 0x1E,
                selected ? 0xED : 0x50,
                selected ? 0xFF : 0xCC);
            SDL_SetRenderDrawColor(r,
                selected ? 0xA7 : 0x78,
                selected ? 0x8B : 0x60,
                selected ? 0xFA : 0xD0,
                selected ? 0xFF : 0x88);
            SDL_Rect br = {bx, row_y, 50, 28};
            SDL_RenderDrawRect(r, &br);
        }
    }

    /* Botão SALVAR */
    int save_x = dx + dw - 130, save_y = dy + dh - 50;
    draw_rounded_rect(r, save_x, save_y, 110, 36, 8,
                      0x7C, 0x3A, 0xED, 0xFF);
    SDL_SetRenderDrawColor(r, COL_TEXT);
    SDL_RenderDrawLine(r,
        save_x + 15, save_y + 18,
        save_x + 95, save_y + 18);

    /* Botão FECHAR (X) */
    int cx = dx + dw - 24, cy2 = dy + 12;
    SDL_SetRenderDrawColor(r, 0xF8, 0x71, 0x71, 0xFF);
    SDL_RenderDrawLine(r, cx-6, cy2-6, cx+6, cy2+6);
    SDL_RenderDrawLine(r, cx+6, cy2-6, cx-6, cy2+6);
}

void sc_drawer_render(sc_drawer *drawer)
{
    SDL_Renderer *r = drawer->renderer;
    int offset = slide_offset(drawer);

    /* ─ Flash de screenshot ─ */
    if (SDL_GetTicks() < drawer->screenshot_flash_until) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, COL_FLASH_OK, 0x55);
        SDL_Rect flash = {0, 0, drawer->win_w, drawer->win_h};
        SDL_RenderFillRect(r, &flash);
    }

    /* ─ Aba toggle (sempre visível no canto direito) ─ */
    int total_w  = TAB_W_PX + DRAWER_W_PX;
    int base_x   = drawer->win_w - TAB_W_PX - offset;

    /* Fundo da aba */
    draw_rounded_rect(r,
        base_x - TAB_W_PX, drawer->win_h/2 - BTN_SIZE,
        TAB_W_PX, BTN_SIZE*2, 8,
        0x50, 0x3C, 0xB4, 0xCC);

    /* Seta na aba */
    SDL_SetRenderDrawColor(r, COL_TEXT);
    int ax = base_x - TAB_W_PX/2;
    int ay = drawer->win_h/2;
    if (drawer->expanded) {
        /* ▶ */
        for (int i=0;i<8;i++) SDL_RenderDrawLine(r,ax+2,ay-i,ax+8,ay);
        for (int i=0;i<8;i++) SDL_RenderDrawLine(r,ax+2,ay+i,ax+8,ay);
    } else {
        /* ◀ */
        for (int i=0;i<8;i++) SDL_RenderDrawLine(r,ax-2,ay-i,ax-8,ay);
        for (int i=0;i<8;i++) SDL_RenderDrawLine(r,ax-2,ay+i,ax-8,ay);
    }

    if (offset > 2) {
        /* ─ Fundo da gaveta ─ */
        int gx = drawer->win_w - offset;
        int gh = drawer->win_h;
        draw_rounded_rect(r, gx, 0, offset, gh, 14,
                          0x0A, 0x08, 0x1A, 0xE8);

        /* Linha separadora à esquerda da gaveta */
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0x64, 0x50, 0xC8, 0x66);
        SDL_RenderDrawLine(r, gx, 0, gx, gh);

        /* ─ Botões ─ */
        float fade = (offset < DRAWER_W_PX) ? (float)offset / DRAWER_W_PX : 1.0f;
        if (fade < 0.3f) goto skip_buttons;

        int order[] = {
            SC_DRAWER_BTN_SCREENSHOT, SC_DRAWER_BTN_AUDIO,
            SC_DRAWER_BTN_VOL_UP, SC_DRAWER_BTN_VOL_DOWN,
            SC_DRAWER_BTN_BACK, SC_DRAWER_BTN_HOME,
            SC_DRAWER_BTN_RECENTS, SC_DRAWER_BTN_SETTINGS
        };
        const char *labels[] = {
            "SS","AU","V+","V-","<","O","#","CF"
        };

        for (int i = 0; i < 8; i++) {
            int idx = order[i];
            SDL_Rect rect = drawer->btn_rects[idx];
            /* Ajusta posição pelo slide */
            rect.x = rect.x - (DRAWER_W_PX - offset);
            if (rect.x < gx) continue;

            bool hover   = (drawer->btn_hover == idx);
            bool special = (idx == SC_DRAWER_BTN_SETTINGS);
            render_button(r, rect, hover, special, labels[i]);
        }

        /* Separadores de seção */
        SDL_SetRenderDrawColor(r, COL_SEP);
        int sep_xs[] = {gx+4, gx+4, gx+4};
        int sep_ys[] = {
            drawer->btn_rects[SC_DRAWER_BTN_AUDIO].y + BTN_SIZE + 3,
            drawer->btn_rects[SC_DRAWER_BTN_VOL_DOWN].y + BTN_SIZE + 3,
            drawer->btn_rects[SC_DRAWER_BTN_RECENTS].y + BTN_SIZE + 6,
        };
        for (int i = 0; i < 3; i++) {
            int sx = drawer->btn_rects[SC_DRAWER_BTN_SCREENSHOT].x - (DRAWER_W_PX - offset);
            SDL_RenderDrawLine(r, sx, sep_ys[i], sx + DRAWER_W_PX - 8, sep_ys[i]);
        }
    }
skip_buttons:

    /* ─ Modal de configurações ─ */
    if (drawer->settings_open) {
        render_settings_dialog(drawer);
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * UPDATE (animação)
 * ───────────────────────────────────────────────────────────────────── */
void sc_drawer_update(sc_drawer *drawer, float delta_ms)
{
    float target = drawer->expanded ? 1.0f : 0.0f;
    float diff   = target - drawer->slide_t;
    if (fabsf(diff) < 0.001f) {
        drawer->slide_t = target;
        return;
    }
    float step = ANIM_SPEED * delta_ms;
    if (fabsf(diff) < step)
        drawer->slide_t = target;
    else
        drawer->slide_t += (diff > 0 ? step : -step);
}

/* ─────────────────────────────────────────────────────────────────────
 * EVENT HANDLING
 * ───────────────────────────────────────────────────────────────────── */

/* Verifica se ponto (px,py) está na aba toggle */
static bool in_tab(sc_drawer *d, int px, int py)
{
    int offset = slide_offset(d);
    int tab_x  = d->win_w - TAB_W_PX - offset;
    return px >= tab_x && px < tab_x + TAB_W_PX &&
           py >= d->win_h/2 - BTN_SIZE &&
           py < d->win_h/2 + BTN_SIZE;
}

/* Verifica se ponto está em algum botão da gaveta */
static int hit_button(sc_drawer *d, int px, int py)
{
    if (!d->expanded && d->slide_t < 0.5f) return -1;
    int offset = slide_offset(d);

    int order[] = {
        SC_DRAWER_BTN_SCREENSHOT, SC_DRAWER_BTN_AUDIO,
        SC_DRAWER_BTN_VOL_UP, SC_DRAWER_BTN_VOL_DOWN,
        SC_DRAWER_BTN_BACK, SC_DRAWER_BTN_HOME,
        SC_DRAWER_BTN_RECENTS, SC_DRAWER_BTN_SETTINGS
    };
    for (int i = 0; i < 8; i++) {
        int idx = order[i];
        SDL_Rect r = d->btn_rects[idx];
        r.x -= (DRAWER_W_PX - offset);
        if (px >= r.x && px < r.x + r.w &&
            py >= r.y && py < r.y + r.h)
            return idx;
    }
    return -1;
}

/* Detecta clique no modal de configurações */
static bool handle_settings_click(sc_drawer *d, int px, int py)
{
    int dw = 320, dh = 280;
    int dx = (d->win_w - dw) / 2;
    int dy = (d->win_h - dh) / 2;

    /* Fora do painel → fecha */
    if (px < dx || px > dx+dw || py < dy || py > dy+dh) {
        d->settings_open = false;
        return true;
    }

    /* Botão fechar (X) */
    if (px >= dx+dw-30 && py <= dy+30) {
        d->settings_open = false;
        return true;
    }

    /* Opções FPS/RES/BPS */
    int *sel[] = {&d->settings_fps_sel, &d->settings_res_sel, &d->settings_bps_sel};
    for (int s = 0; s < 3; s++) {
        int row_y = dy + 50 + s * 64;
        for (int i = 0; i < OPT_COUNT; i++) {
            int bx = dx + 80 + i * 56;
            if (px >= bx && px < bx+50 && py >= row_y && py < row_y+28) {
                *sel[s] = i;
                return true;
            }
        }
    }

    /* Botão SALVAR */
    int save_x = dx + dw - 130, save_y = dy + dh - 50;
    if (px >= save_x && px < save_x+110 && py >= save_y && py < save_y+36) {
        /* Aplica seleções ao cfg */
        d->cfg.max_fps    = FPS_OPTIONS[d->settings_fps_sel];
        d->cfg.resolution = RES_OPTIONS[d->settings_res_sel];
        strncpy(d->cfg.bitrate, BPS_OPTIONS[d->settings_bps_sel],
                sizeof(d->cfg.bitrate)-1);

        sc_drawer_save_config(&d->cfg);
        d->settings_open = false;

        /* Reinicia scrcpy com nova config */
        sc_drawer_restart_scrcpy(d);
        return true;
    }

    return true; /* consome clique dentro do painel */
}

bool sc_drawer_handle_event(sc_drawer *drawer, const SDL_Event *event)
{
    switch (event->type) {

    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            sc_drawer_resize(drawer,
                event->window.data1, event->window.data2);
        }
        return false; /* não consome resize */

    case SDL_MOUSEMOTION: {
        int px = event->motion.x, py = event->motion.y;
        int hit = hit_button(drawer, px, py);
        drawer->btn_hover = hit;
        /* Consome se o mouse está sobre a gaveta */
        int offset = slide_offset(drawer);
        if (offset > 2 && px >= drawer->win_w - offset - TAB_W_PX)
            return true;
        if (in_tab(drawer, px, py)) return true;
        return false;
    }

    case SDL_MOUSEBUTTONDOWN: {
        if (event->button.button != SDL_BUTTON_LEFT) return false;
        int px = event->button.x, py = event->button.y;

        /* Modal de configurações tem prioridade */
        if (drawer->settings_open) {
            return handle_settings_click(drawer, px, py);
        }

        /* Aba toggle */
        if (in_tab(drawer, px, py)) {
            drawer->expanded = !drawer->expanded;
            return true;
        }

        /* Botões da gaveta */
        int btn = hit_button(drawer, px, py);
        if (btn < 0) return false;

        switch (btn) {
        case SC_DRAWER_BTN_SCREENSHOT:
            sc_drawer_action_screenshot(drawer);
            break;
        case SC_DRAWER_BTN_AUDIO:
            drawer->cfg.no_audio = !drawer->cfg.no_audio;
            sc_drawer_restart_scrcpy(drawer);
            break;
        case SC_DRAWER_BTN_VOL_UP:
            sc_drawer_action_volume(drawer, true);
            break;
        case SC_DRAWER_BTN_VOL_DOWN:
            sc_drawer_action_volume(drawer, false);
            break;
        case SC_DRAWER_BTN_BACK:
            sc_drawer_action_nav(drawer, KEYCODE_BACK);
            break;
        case SC_DRAWER_BTN_HOME:
            sc_drawer_action_nav(drawer, KEYCODE_HOME);
            break;
        case SC_DRAWER_BTN_RECENTS:
            sc_drawer_action_nav(drawer, KEYCODE_APP_SWITCH);
            break;
        case SC_DRAWER_BTN_SETTINGS:
            drawer->settings_open = true;
            break;
        }
        return true;
    }

    case SDL_MOUSEBUTTONUP:
        if (drawer->settings_open) return true;
        {
            int px = event->button.x, py = event->button.y;
            int offset = slide_offset(drawer);
            if (offset > 2 && px >= drawer->win_w - offset - TAB_W_PX)
                return true;
        }
        return false;
    }

    return false;
}
