#include "favorites.h"

#include <glib/gstdio.h>
#include <errno.h>

static const gchar *non_null(const gchar *value) { return value ? value : ""; }

static gboolean save_books(const gchar *path, GPtrArray *books, GError **error) {
    GKeyFile *file = g_key_file_new();
    gchar *directory = g_path_get_dirname(path);
    gchar *contents;
    gsize length;
    guint i;
    gboolean saved;

    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Не удалось создать папку избранного: %s", g_strerror(errno));
        g_free(directory);
        g_key_file_free(file);
        return FALSE;
    }
    g_free(directory);
    g_key_file_set_integer(file, "favorites", "count", books->len);
    for (i = 0; i < books->len; i++) {
        BookRelayBook *book = g_ptr_array_index(books, i);
        gchar *group = g_strdup_printf("book-%u", i);
        g_key_file_set_string(file, group, "id", non_null(book->id));
        g_key_file_set_string(file, group, "title", non_null(book->title));
        g_key_file_set_string(file, group, "author", non_null(book->author));
        g_key_file_set_string(file, group, "cover_url", non_null(book->cover_url));
        g_key_file_set_string(file, group, "description", non_null(book->description));
        g_key_file_set_string(file, group, "translator", non_null(book->translator));
        g_key_file_set_integer(file, group, "year", book->year);
        g_free(group);
    }
    contents = g_key_file_to_data(file, &length, error);
    saved = contents && g_file_set_contents(path, contents, (gssize)length, error);
    g_free(contents);
    g_key_file_free(file);
    return saved;
}

BookRelayFavorites *bookrelay_favorites_load(const gchar *config_path, const gchar *relay_url) {
    BookRelayFavorites *favorites = g_new0(BookRelayFavorites, 1);
    gchar *directory = g_path_get_dirname(config_path);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, non_null(relay_url), -1);
    gchar *filename = g_strdup_printf("favorites-%s.ini", hash);
    GKeyFile *file = g_key_file_new();
    GError *error = NULL;
    gint count, i;

    favorites->path = g_build_filename(directory, filename, NULL);
    favorites->books = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_book_free);
    g_free(directory); g_free(hash); g_free(filename);
    if (!g_key_file_load_from_file(file, favorites->path, G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("Избранное не загружено: %s", error->message);
        g_clear_error(&error);
        g_key_file_free(file);
        return favorites;
    }
    count = g_key_file_get_integer(file, "favorites", "count", NULL);
    for (i = 0; i < count && i < 10000; i++) {
        gchar *group = g_strdup_printf("book-%d", i);
        BookRelayBook *book = g_new0(BookRelayBook, 1);
        book->id = g_key_file_get_string(file, group, "id", NULL);
        book->title = g_key_file_get_string(file, group, "title", NULL);
        book->author = g_key_file_get_string(file, group, "author", NULL);
        book->cover_url = g_key_file_get_string(file, group, "cover_url", NULL);
        book->description = g_key_file_get_string(file, group, "description", NULL);
        book->translator = g_key_file_get_string(file, group, "translator", NULL);
        book->year = g_key_file_get_integer(file, group, "year", NULL);
        if (book->id && *book->id && !bookrelay_favorites_contains(favorites, book->id))
            g_ptr_array_add(favorites->books, book);
        else bookrelay_book_free(book);
        g_free(group);
    }
    g_key_file_free(file);
    return favorites;
}

gboolean bookrelay_favorites_contains(const BookRelayFavorites *favorites, const gchar *book_id) {
    guint i;
    if (!favorites || !book_id || !*book_id) return FALSE;
    for (i = 0; i < favorites->books->len; i++) {
        BookRelayBook *book = g_ptr_array_index(favorites->books, i);
        if (g_strcmp0(book->id, book_id) == 0) return TRUE;
    }
    return FALSE;
}

gboolean bookrelay_favorites_set(BookRelayFavorites *favorites, const BookRelayBook *book,
                                 gboolean selected, GError **error) {
    GPtrArray *updated;
    GPtrArray *old;
    guint i;
    gboolean saved;
    if (!favorites || !book || !book->id || !*book->id) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Книга не имеет идентификатора");
        return FALSE;
    }
    if (bookrelay_favorites_contains(favorites, book->id) == selected) return TRUE;
    updated = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_book_free);
    for (i = 0; i < favorites->books->len; i++) {
        BookRelayBook *item = g_ptr_array_index(favorites->books, i);
        if (g_strcmp0(item->id, book->id) != 0)
            g_ptr_array_add(updated, bookrelay_book_copy(item));
    }
    if (selected) g_ptr_array_add(updated, bookrelay_book_copy(book));
    saved = save_books(favorites->path, updated, error);
    if (saved) {
        old = favorites->books;
        favorites->books = updated;
        g_ptr_array_free(old, TRUE);
    } else g_ptr_array_free(updated, TRUE);
    return saved;
}

void bookrelay_favorites_free(BookRelayFavorites *favorites) {
    if (!favorites) return;
    g_ptr_array_free(favorites->books, TRUE);
    g_free(favorites->path);
    g_free(favorites);
}
