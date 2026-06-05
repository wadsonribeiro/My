/**
 * sc_drawer.h — Gaveta retrátil lateral integrada no SDL2 do scrcpy
 *
 * Adicione ao build: meson.build -> app/src/sc_drawer.c
 * Include em screen.c e scrcpy.c
 *
 * Compilação: parte do fork wadsonribeiro/My
 * CI/CD: .github/workflows/build-windows.yml
 */

#ifndef SC_DRAWER_H
#define SC_DRAWER_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

/* ─── Config do dispositivo ────────────────────────────────────────── */
#define SC_DRAWER_MAX_ID_LEN  64
#define SC_DRAWER_CONFIG_FILE "devices_data.json"
#define SC_DRAWER_PRINTS_DIR  "Prints"

/* Configuração por dispositivo (espelhado do JSON) */
typedef struct {
    char   device_id[SC_DRAWER_MAX_ID_LEN];
    int    max_fps;      /* 30 | 60 | 120 | 240        */
    int    resolution;   /* 540 | 720 | 1080 | 1440     */
    char   bitrate[8];   /* "4M" | "8M" | "16M" | "32M" */
    bool   no_audio;
} sc_device_config;

/* ─── Estado da gaveta ─────────────────────────────────────────────── */
#define SC_DRAWER_BTN_COUNT 9

typedef enum {
    SC_DRAWER_BTN_SCREENSHOT = 0,
    SC_DRAWER_BTN_AUDIO,
    SC_DRAWER_BTN_VOL_UP,
    SC_DRAWER_BTN_VOL_DOWN,
    SC_DRAWER_BTN_BACK,
    SC_DRAWER_BTN_HOME,
    SC_DRAWER_BTN_RECENTS,
    SC_DRAWER_BTN_SETTINGS,
    SC_DRAWER_BTN_TOGGLE,   /* a aba de abrir/fechar */
} sc_drawer_btn;

typedef struct {
    /* SDL */
    SDL_Window   *window;
    SDL_Renderer *renderer;

    /* Geometria */
    int win_w, win_h;       /* dimensões da janela scrcpy */
    int drawer_w;           /* largura da gaveta expandida */
    int tab_w;              /* largura da aba toggle       */
    bool expanded;          /* gaveta aberta?              */
    float slide_t;          /* 0.0 = fechada, 1.0 = aberta (animação) */

    /* Botões — rects calculados dinamicamente */
    SDL_Rect btn_rects[SC_DRAWER_BTN_COUNT];
    int      btn_hover;     /* índice do botão em hover, -1 se nenhum */

    /* Config do dispositivo ativo */
    sc_device_config cfg;

    /* Janela de configurações (modal simples SDL) */
    bool settings_open;
    int  settings_fps_sel;  /* índice selecionado */
    int  settings_res_sel;
    int  settings_bps_sel;

    /* Fontes SDL_ttf (opcional — usa emojis/texto simples sem TTF) */
    /* TTF_Font *font; */

    /* Screenshot em andamento */
    bool screenshot_pending;
    uint32_t screenshot_flash_until; /* SDL_GetTicks até quando mostrar flash */

} sc_drawer;

/* ─── API ──────────────────────────────────────────────────────────── */

/**
 * Inicializa a gaveta.
 * Deve ser chamado após sc_screen_init(), passando a SDL_Window e renderer.
 */
bool sc_drawer_init(sc_drawer *drawer,
                    SDL_Window *window,
                    SDL_Renderer *renderer,
                    const char *device_id);

/** Libera recursos. */
void sc_drawer_destroy(sc_drawer *drawer);

/**
 * Atualiza geometria quando a janela for redimensionada.
 * Chame em SDL_WINDOWEVENT_SIZE_CHANGED.
 */
void sc_drawer_resize(sc_drawer *drawer, int new_w, int new_h);

/**
 * Processa eventos SDL (mouse, teclado).
 * Retorna true se o evento foi consumido pela gaveta (não repassar ao scrcpy).
 */
bool sc_drawer_handle_event(sc_drawer *drawer, const SDL_Event *event);

/**
 * Atualiza animações (deve ser chamado a cada frame, antes de render).
 * delta_ms: tempo desde o último frame em ms.
 */
void sc_drawer_update(sc_drawer *drawer, float delta_ms);

/**
 * Renderiza a gaveta sobre o frame do scrcpy.
 * Chame APÓS sc_screen_render(), antes de SDL_RenderPresent.
 */
void sc_drawer_render(sc_drawer *drawer);

/* ─── Config I/O ───────────────────────────────────────────────────── */

/** Carrega config do arquivo JSON para o device_id especificado. */
bool sc_drawer_load_config(sc_device_config *cfg, const char *device_id);

/** Salva config no arquivo JSON, vinculado ao device_id. */
bool sc_drawer_save_config(const sc_device_config *cfg);

/** Preenche cfg com os valores padrão. */
void sc_drawer_default_config(sc_device_config *cfg, const char *device_id);

/* ─── Ações ────────────────────────────────────────────────────────── */

/** Tira screenshot via ADB e salva em Prints/. */
void sc_drawer_action_screenshot(sc_drawer *drawer);

/** Liga/desliga áudio (reinicia scrcpy). */
void sc_drawer_action_toggle_audio(sc_drawer *drawer);

/** Volume up/down via ADB keyevent. */
void sc_drawer_action_volume(sc_drawer *drawer, bool up);

/** Botões de navegação Android via ADB keyevent. */
void sc_drawer_action_nav(sc_drawer *drawer, int keycode);

/**
 * Reinicia scrcpy com as configurações atuais de cfg.
 * Usado após salvar configurações.
 */
void sc_drawer_restart_scrcpy(sc_drawer *drawer);

#endif /* SC_DRAWER_H */
