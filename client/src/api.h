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
    gchar *token;
    gchar *kindle_email;
} BookRelayClaim;

typedef struct {
    gchar *id;
    gchar *title;
} BookRelayCategory;

GPtrArray *bookrelay_api_search(const gchar *base_url, const gchar *token, const gchar *query, const gchar *category, gint page, gint size, gboolean *has_next, GError **error);
GPtrArray *bookrelay_api_categories(const gchar *base_url, const gchar *token, GError **error);
GPtrArray *bookrelay_api_subcategories(const gchar *base_url, const gchar *token, const gchar *category, GError **error);
GPtrArray *bookrelay_api_catalog_books(const gchar *base_url, const gchar *token, const gchar *category, const gchar *subcategory, gint page, gint size, gboolean *has_next, GError **error);
BookRelayBook *bookrelay_api_book(const gchar *base_url, const gchar *token, const gchar *book_id, GError **error);
BookRelayClaim *bookrelay_api_pair_claim(const gchar *base_url, const gchar *code, GError **error);
gchar *bookrelay_api_update_email(const gchar *base_url, const gchar *token, const gchar *email, GError **error);
gchar *bookrelay_api_send(const gchar *base_url, const gchar *token, const gchar *book_id, const gchar *title, GError **error);
gchar *bookrelay_api_delivery_status(const gchar *base_url, const gchar *token, const gchar *job_id, GError **error);
gboolean bookrelay_api_download(const gchar *base_url, const gchar *token, const gchar *url, GByteArray **payload, GError **error);
void bookrelay_book_free(BookRelayBook *book);
BookRelayBook *bookrelay_book_copy(const BookRelayBook *book);
void bookrelay_claim_free(BookRelayClaim *claim);
void bookrelay_category_free(BookRelayCategory *category);

#endif
