#include "i18n.h"

#include <stdatomic.h>

/* Catalog and download workers can format messages while Settings is open. */
static atomic_int language = UI_UKRAINIAN;

UiLanguage ui_language(void) {
    return atomic_load_explicit(&language, memory_order_relaxed);
}

void ui_set_language(UiLanguage selected) {
    atomic_store_explicit(&language, selected, memory_order_relaxed);
}
