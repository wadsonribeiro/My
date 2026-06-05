#ifndef SC_SIDEBAR_H
#define SC_SIDEBAR_H

#include "common.h"
#include <stdbool.h>
#include <SDL3/SDL.h>

#define SIDEBAR_WIDTH 52

typedef enum {
    SC_SIDEBAR_BTN_SCREENSHOT = 0,
    SC_SIDEBAR_BTN_AUDIO,
    SC_SIDEBAR_BTN_VOL_DOWN,
    SC_SIDEBAR_BTN_VOL_UP,
    SC_SIDEBAR_BTN_BACK,
    SC_SIDEBAR_BTN_HOME,
    SC_SIDEBAR_BTN_APP_SWITCH,
    SC_SIDEBAR_BTN_COUNT,
} sc_sidebar_btn;

struct sc_sidebar {
    SDL_Renderer *renderer;
    int x;          // posição x da barra (lado direito do vídeo)
    int height;     // altura da janela
    bool audio_muted;

    // rects calculados para cada botão
    SDL_FRect btn_rects[SC_SIDEBAR_BTN_COUNT];
};

// Inicializa a sidebar
void sc_sidebar_init(struct sc_sidebar *sidebar, SDL_Renderer *renderer);

// Atualiza posição e tamanho quando a janela muda
void sc_sidebar_update_layout(struct sc_sidebar *sidebar, int x, int height);

// Renderiza a sidebar
void sc_sidebar_render(struct sc_sidebar *sidebar);

// Retorna qual botão foi clicado (ou -1 se nenhum)
int sc_sidebar_get_button_at(struct sc_sidebar *sidebar, float px, float py);

// Alterna estado de mudo do áudio
void sc_sidebar_toggle_audio(struct sc_sidebar *sidebar);

#endif // SC_SIDEBAR_H
