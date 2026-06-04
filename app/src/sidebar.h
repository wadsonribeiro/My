#ifndef SIDEBAR_H
#define SIDEBAR_H

#include <stdbool.h>
#include <SDL3/SDL.h>

/* Largura da barra lateral em pixels lógicos */
#define SIDEBAR_WIDTH 56

/* IDs dos botões */
typedef enum {
    SIDEBAR_BTN_SCREENSHOT = 0,
    SIDEBAR_BTN_AUDIO,
    SIDEBAR_BTN_VOL_DOWN,
    SIDEBAR_BTN_VOL_UP,
    SIDEBAR_BTN_BACK,
    SIDEBAR_BTN_HOME,
    SIDEBAR_BTN_RECENTS,
    SIDEBAR_BTN_COUNT,
} sc_sidebar_btn_id;

struct sc_sidebar_btn {
    SDL_FRect rect;       /* posição na janela */
    const char *label;    /* emoji / texto */
    bool pressed;
};

struct sc_sidebar {
    bool enabled;
    bool audio_on;        /* estado do toggle de áudio */
    struct sc_sidebar_btn buttons[SIDEBAR_BTN_COUNT];
    SDL_Texture *tex;     /* textura de fundo (opcional) */
};

/* Inicializa a sidebar */
void sc_sidebar_init(struct sc_sidebar *sb);

/* Atualiza posições dos botões quando a janela redimensiona */
void sc_sidebar_layout(struct sc_sidebar *sb, int win_w, int win_h);

/* Desenha a sidebar no renderer */
void sc_sidebar_render(struct sc_sidebar *sb, SDL_Renderer *renderer);

/* Processa um clique. Retorna o id do botão clicado ou -1 */
int sc_sidebar_handle_click(struct sc_sidebar *sb, float x, float y);

/* Retorna true se o ponto (x,y) está dentro da sidebar */
bool sc_sidebar_contains(struct sc_sidebar *sb, float x, float y);

#endif /* SIDEBAR_H */
