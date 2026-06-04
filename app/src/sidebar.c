#include "sidebar.h"

#include <string.h>

/* Cores */
#define COL_BG      { 22,  22,  22, 255 }
#define COL_BTN     { 42,  42,  42, 255 }
#define COL_HOVER   { 60,  60,  60, 255 }
#define COL_PRESS   { 25,  118, 210, 255 }
#define COL_AUDIO_ON  { 0, 121, 107, 255 }

/* Altura de cada botão e espaçamento */
#define BTN_H    48
#define BTN_PAD   6
#define BTN_SEP  12   /* separador extra entre grupos */

static const char *LABELS[SIDEBAR_BTN_COUNT] = {
    /* SCREENSHOT */ "SS",
    /* AUDIO      */ "AU",
    /* VOL_DOWN   */ "V-",
    /* VOL_UP     */ "V+",
    /* BACK       */ "<-",
    /* HOME       */ "HM",
    /* RECENTS    */ "[]",
};

void sc_sidebar_init(struct sc_sidebar *sb) {
    memset(sb, 0, sizeof(*sb));
    sb->enabled  = true;
    sb->audio_on = true;
    for (int i = 0; i < SIDEBAR_BTN_COUNT; i++) {
        sb->buttons[i].label = LABELS[i];
    }
}

void sc_sidebar_layout(struct sc_sidebar *sb, int win_w, int win_h) {
    (void) win_h;
    float x   = (float)(win_w - SIDEBAR_WIDTH) + BTN_PAD;
    float bw  = (float)(SIDEBAR_WIDTH - BTN_PAD * 2);
    float y   = (float)BTN_PAD;

    /* Separadores extras: após AUDIO(1) e após VOL_UP(3) */
    for (int i = 0; i < SIDEBAR_BTN_COUNT; i++) {
        if (i == 2 || i == 4)   /* antes de VOL_DOWN e BACK */
            y += BTN_SEP;

        sb->buttons[i].rect.x = x;
        sb->buttons[i].rect.y = y;
        sb->buttons[i].rect.w = bw;
        sb->buttons[i].rect.h = BTN_H;
        y += BTN_H + BTN_PAD;
    }
}

void sc_sidebar_render(struct sc_sidebar *sb, SDL_Renderer *renderer) {
    if (!sb->enabled) return;

    /* Fundo da sidebar — será desenhado em screen.c reservando a região */
    for (int i = 0; i < SIDEBAR_BTN_COUNT; i++) {
        struct sc_sidebar_btn *btn = &sb->buttons[i];

        SDL_Color c;
        if (btn->pressed) {
            SDL_Color p = COL_PRESS;
            c = p;
        } else if (i == SIDEBAR_BTN_AUDIO && sb->audio_on) {
            SDL_Color a = COL_AUDIO_ON;
            c = a;
        } else {
            SDL_Color b = COL_BTN;
            c = b;
        }

        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(renderer, &btn->rect);

        /* Borda sutil */
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        SDL_RenderRect(renderer, &btn->rect);
    }
}

int sc_sidebar_handle_click(struct sc_sidebar *sb, float x, float y) {
    for (int i = 0; i < SIDEBAR_BTN_COUNT; i++) {
        SDL_FRect *r = &sb->buttons[i].rect;
        if (x >= r->x && x < r->x + r->w &&
            y >= r->y && y < r->y + r->h) {

            /* Toggle estado de áudio */
            if (i == SIDEBAR_BTN_AUDIO)
                sb->audio_on = !sb->audio_on;

            return i;
        }
    }
    return -1;
}

bool sc_sidebar_contains(struct sc_sidebar *sb, float x, float y) {
    /* Verifica se o ponto está na coluna da sidebar */
    if (!sb->enabled) return false;
    /* Usa a posição do primeiro botão para descobrir o limite esquerdo */
    return (x >= sb->buttons[0].rect.x - BTN_PAD);
}
