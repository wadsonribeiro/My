#include "sidebar.h"
#include <string.h>
#include "util/log.h"

#define BTN_HEIGHT  52
#define BTN_MARGIN  2
#define BTN_RADIUS  6.0f

// Cores
#define COL_BG       0x1A, 0x1A, 0x2E, 0xE8  // fundo escuro semi-transparente
#define COL_BTN      0x16, 0x21, 0x3E, 0xFF  // botão normal azul escuro
#define COL_BTN_RED  0x8B, 0x00, 0x00, 0xFF  // botão vermelho (mudo)
#define COL_DIVIDER  0x30, 0x30, 0x50, 0xFF  // linha divisória

// Símbolos dos botões (UTF-8 não funciona em SDL sem SDL_ttf, usamos labels simples)
static const char *BTN_LABELS[SC_SIDEBAR_BTN_COUNT] = {
    "SS",   // Screenshot
    "AU",   // Audio
    "V-",   // Volume Down
    "V+",   // Volume Up
    "<-",   // Back
    "HM",   // Home
    "[]",   // App Switch
};

void sc_sidebar_init(struct sc_sidebar *sidebar, SDL_Renderer *renderer) {
    memset(sidebar, 0, sizeof(*sidebar));
    sidebar->renderer = renderer;
    sidebar->audio_muted = false;
}

void sc_sidebar_update_layout(struct sc_sidebar *sidebar, int x, int height) {
    sidebar->x = x;
    sidebar->height = height;

    float y = 10.0f;
    float bw = (float)(SIDEBAR_WIDTH - BTN_MARGIN * 2);

    for (int i = 0; i < SC_SIDEBAR_BTN_COUNT; i++) {
        // Linha divisória antes dos botões de navegação
        if (i == SC_SIDEBAR_BTN_BACK) {
            y += 8.0f;
        }
        sidebar->btn_rects[i] = (SDL_FRect){
            .x = (float)x + BTN_MARGIN,
            .y = y,
            .w = bw,
            .h = (float)(BTN_HEIGHT - BTN_MARGIN * 2),
        };
        y += BTN_HEIGHT;
    }
}

static void draw_rounded_rect(SDL_Renderer *r, const SDL_FRect *rect, float radius) {
    // SDL3 não tem rounded rect nativo sem SDL_gfx; usamos rect simples
    (void)radius;
    SDL_RenderFillRect(r, rect);
}

void sc_sidebar_render(struct sc_sidebar *sidebar) {
    SDL_Renderer *r = sidebar->renderer;

    // Fundo da sidebar
    SDL_SetRenderDrawColor(r, COL_BG);
    SDL_FRect bg = {
        .x = (float)sidebar->x,
        .y = 0,
        .w = (float)SIDEBAR_WIDTH,
        .h = (float)sidebar->height,
    };
    SDL_RenderFillRect(r, &bg);

    // Linha divisória esquerda
    SDL_SetRenderDrawColor(r, COL_DIVIDER);
    SDL_FRect divider = { .x = (float)sidebar->x, .y = 0, .w = 1, .h = (float)sidebar->height };
    SDL_RenderFillRect(r, &divider);

    // Botões
    for (int i = 0; i < SC_SIDEBAR_BTN_COUNT; i++) {
        // Linha divisória antes dos botões de navegação
        if (i == SC_SIDEBAR_BTN_BACK) {
            SDL_SetRenderDrawColor(r, COL_DIVIDER);
            SDL_FRect div = {
                .x = (float)sidebar->x + 6,
                .y = sidebar->btn_rects[i].y - 5,
                .w = (float)(SIDEBAR_WIDTH - 12),
                .h = 1,
            };
            SDL_RenderFillRect(r, &div);
        }

        // Cor do botão de áudio muda quando mudo
        if (i == SC_SIDEBAR_BTN_AUDIO && sidebar->audio_muted) {
            SDL_SetRenderDrawColor(r, COL_BTN_RED);
        } else {
            SDL_SetRenderDrawColor(r, COL_BTN);
        }

        draw_rounded_rect(r, &sidebar->btn_rects[i], BTN_RADIUS);

        // Label do botão (sem SDL_ttf usamos debug simples; integração real precisa SDL_ttf)
        // Por ora só o retângulo colorido serve como botão funcional
        // Para texto, seria necessário SDL_ttf ou um atlas de sprites
        (void)BTN_LABELS[i]; // evita warning de unused
    }
}

int sc_sidebar_get_button_at(struct sc_sidebar *sidebar, float px, float py) {
    for (int i = 0; i < SC_SIDEBAR_BTN_COUNT; i++) {
        const SDL_FRect *r = &sidebar->btn_rects[i];
        if (px >= r->x && px <= r->x + r->w &&
            py >= r->y && py <= r->y + r->h) {
            return i;
        }
    }
    return -1;
}

void sc_sidebar_toggle_audio(struct sc_sidebar *sidebar) {
    sidebar->audio_muted = !sidebar->audio_muted;
}
