#ifndef BOOKRELAY_API_H
#define BOOKRELAY_API_H

#include <glib.h>

typedef struct {
    gchar *id;
    gchar *title;
    gchar *author;
    gchar *cover_url;
    gchar *description;
    gchar *translator;
    gint year;
} BookRelayBook;

typedef struct {
    gchar *code;
    gchar *expires_at;
} BookRelayPairing;

GPtrArray *bookrelay_api_search(const gchar *base_url, const gchar *token, const gchar *query, gint page, GError **error);
GPtrArray *bookrelay_api_categories(const gchar *base_url, const gchar *token, GError **error);
BookRelayBook *bookrelay_api_book(const gchar *base_url, const gchar *token, const gchar *book_id, GError **error);
BookRelayPairing *bookrelay_api_start_pairing(const gchar *base_url, const gchar *device_id, GError **error);
gchar *bookrelay_api_pair_status(const gchar *base_url, const gchar *code, gchar **kindle_email, GError **error);
gchar *bookrelay_api_send(const gchar *base_url, const gchar *token, const gchar *book_id, const gchar *title, GError **error);
gchar *bookrelay_api_delivery_status(const gchar *base_url, const gchar *token, const gchar *job_id, GError **error);
gboolean bookrelay_api_download(const gchar *url, GBytes **payload, GError **error);
void bookrelay_book_free(BookRelayBook *book);
BookRelayBook *bookrelay_book_copy(const BookRelayBook *book);
void bookrelay_pairing_free(BookRelayPairing *pairing);

#endif
