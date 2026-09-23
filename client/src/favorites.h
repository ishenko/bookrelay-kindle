#ifndef BOOKRELAY_FAVORITES_H
#define BOOKRELAY_FAVORITES_H

#include "api.h"

typedef struct {
    gchar *path;
    GPtrArray *books;
} BookRelayFavorites;

BookRelayFavorites *bookrelay_favorites_load(const gchar *config_path, const gchar *relay_url);
gboolean bookrelay_favorites_contains(const BookRelayFavorites *favorites, const gchar *book_id);
gboolean bookrelay_favorites_set(BookRelayFavorites *favorites, const BookRelayBook *book,
                                 gboolean selected, GError **error);
void bookrelay_favorites_free(BookRelayFavorites *favorites);

#endif
