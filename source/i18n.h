#ifndef UAPLAY_I18N_H
#define UAPLAY_I18N_H

typedef enum { UI_UKRAINIAN, UI_ENGLISH } UiLanguage;

typedef struct {
    const char *uk;
    const char *en;
} UiText;

UiLanguage ui_language(void);
void ui_set_language(UiLanguage language);

#define TR(uk, en) (ui_language() == UI_ENGLISH ? (en) : (uk))

static inline const char *ui_text(UiText text) {
    return TR(text.uk, text.en);
}

#endif
