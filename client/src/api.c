#include "api.h"

#include <curl/curl.h>
#include <string.h>

#define API_ERROR g_quark_from_static_string("bookrelay-api-error")
#define MAX_API_RESPONSE_BYTES (2u * 1024u * 1024u)
#define MAX_COVER_RESPONSE_BYTES (8u * 1024u * 1024u)

typedef struct {
    GByteArray *data;
    gsize max_bytes;
    gboolean exceeded;
} Response;

static size_t receive_body(char *ptr, size_t size, size_t count, void *userdata) {
    Response *response = userdata;
    gsize incoming = size * count;
    if (incoming > response->max_bytes - response->data->len) {
        response->exceeded = TRUE;
        return 0;
    }
    g_byte_array_append(response->data, (guint8 *)ptr, incoming);
    return size * count;
}

static gchar *url_encode(const gchar *value) {
    CURL *curl = curl_easy_init();
    gchar *encoded;
    gchar *result;
    if (!curl) return g_strdup(value);
    encoded = curl_easy_escape(curl, value, 0);
    result = g_strdup(encoded ? encoded : value);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

static gboolean request_bytes(const gchar *method, const gchar *url, const gchar *token, const gchar *body, gsize max_bytes, GByteArray **payload, long *status, GError **error) {
    CURL *curl = curl_easy_init();
    Response response = { g_byte_array_new(), max_bytes, FALSE };
    struct curl_slist *headers = NULL;
    CURLcode result;
    gboolean ok = FALSE;

    if (!curl) {
        g_set_error(error, API_ERROR, 1, "libcurl initialization failed");
        g_byte_array_free(response.data, TRUE);
        return FALSE;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_bytes);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BookRelay Kindle/0.1");
    if (g_strcmp0(method, "POST") == 0 || g_strcmp0(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        if (g_strcmp0(method, "PUT") == 0)
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        else
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    if (token && *token) {
        gchar *authorization = g_strdup_printf("Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, authorization);
        g_free(authorization);
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    if (response.exceeded) {
        g_set_error(error, API_ERROR, 3, "relay response exceeded %u bytes", (guint)max_bytes);
    } else if (result != CURLE_OK) {
        g_set_error(error, API_ERROR, 2, "network request failed: %s", curl_easy_strerror(result));
    } else if (*status < 200 || *status >= 300) {
        gchar *detail = g_strndup((const gchar *)response.data->data, MIN(response.data->len, (gsize)240));
        g_strstrip(detail);
        if (*detail) {
            g_set_error(error, API_ERROR, (gint)*status, "relay returned HTTP %ld: %s", *status, detail);
        } else {
            g_set_error(error, API_ERROR, (gint)*status, "relay returned HTTP %ld", *status);
        }
        g_free(detail);
    } else {
        *payload = response.data;
        response.data = NULL;
        ok = TRUE;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (response.data) g_byte_array_free(response.data, TRUE);
    return ok;
}

static gchar *request(const gchar *method, const gchar *url, const gchar *token, const gchar *body, long *status, GError **error) {
    GByteArray *payload = NULL;
    gchar *output;
    if (!request_bytes(method, url, token, body, MAX_API_RESPONSE_BYTES, &payload, status, error)) return NULL;
    output = g_strndup((gchar *)payload->data, payload->len);
    g_byte_array_free(payload, TRUE);
    return output;
}

static gchar *json_unescape(const gchar *value, gsize length) {
    GString *result = g_string_new(NULL);
    gsize i;
    for (i = 0; i < length; i++) {
        if (value[i] == '\\' && i + 1 < length) {
            i++;
            switch (value[i]) {
                case 'n': g_string_append_c(result, '\n'); break;
                case 'r': g_string_append_c(result, '\r'); break;
                case 't': g_string_append_c(result, '\t'); break;
                case '"': g_string_append_c(result, '"'); break;
                case '\\': g_string_append_c(result, '\\'); break;
                default: g_string_append_c(result, value[i]); break;
            }
        } else {
            g_string_append_c(result, value[i]);
        }
    }
    return g_string_free(result, FALSE);
}

static gchar *json_escape(const gchar *value) {
    GString *result = g_string_new(NULL);
    const gchar *cursor;
    for (cursor = value; cursor && *cursor; cursor++) {
        switch (*cursor) {
            case '"': g_string_append(result, "\\\""); break;
            case '\\': g_string_append(result, "\\\\"); break;
            case '\n': g_string_append(result, "\\n"); break;
            case '\r': g_string_append(result, "\\r"); break;
            case '\t': g_string_append(result, "\\t"); break;
            default: g_string_append_c(result, *cursor); break;
        }
    }
    return g_string_free(result, FALSE);
}

static gchar *json_string(const gchar *object, const gchar *key) {
    gchar *needle = g_strdup_printf("\"%s\"", key);
    const gchar *start = strstr(object, needle);
    const gchar *cursor;
    GString *raw;
    gchar *result;
    g_free(needle);
    if (!start) return g_strdup("");
    cursor = start + strlen(key) + 2;
    while (g_ascii_isspace(*cursor)) cursor++;
    if (*cursor++ != ':') return g_strdup("");
    while (g_ascii_isspace(*cursor)) cursor++;
    if (*cursor++ != '"') return g_strdup("");
    raw = g_string_new(NULL);
    while (*cursor) {
        if (*cursor == '"' && (raw->len == 0 || raw->str[raw->len - 1] != '\\')) break;
        g_string_append_c(raw, *cursor++);
    }
    result = json_unescape(raw->str, raw->len);
    g_string_free(raw, TRUE);
    return result;
}

static gint json_int(const gchar *object, const gchar *key) {
    gchar *needle = g_strdup_printf("\"%s\":", key);
    const gchar *found = strstr(object, needle);
    gint value = found ? (gint)g_ascii_strtoll(found + strlen(needle), NULL, 10) : 0;
    g_free(needle);
    return value;
}

static GPtrArray *parse_books(const gchar *json) {
    GPtrArray *books = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_book_free);
    const gchar *cursor = json;
    while ((cursor = strstr(cursor, "\"id\":\"")) != NULL) {
        const gchar *end = strchr(cursor, '}');
        gchar *object;
        BookRelayBook *book;
        if (!end) break;
        object = g_strndup(cursor, (gsize)(end - cursor));
        book = g_new0(BookRelayBook, 1);
        book->id = json_string(object, "id");
        book->title = json_string(object, "title");
        book->author = json_string(object, "author");
        book->cover_url = json_string(object, "cover_url");
        book->description = json_string(object, "description");
        book->translator = json_string(object, "translator");
        book->year = json_int(object, "year");
        g_ptr_array_add(books, book);
        g_free(object);
        cursor = end + 1;
    }
    return books;
}

static gchar *join_url(const gchar *base, const gchar *path) {
    gchar *trimmed = g_strdup(base);
    gchar *url;
    while (strlen(trimmed) && trimmed[strlen(trimmed) - 1] == '/') trimmed[strlen(trimmed) - 1] = 0;
    url = g_strdup_printf("%s/%s", trimmed, path[0] == '/' ? path + 1 : path);
    g_free(trimmed);
    return url;
}

GPtrArray *bookrelay_api_search(const gchar *base_url, const gchar *token, const gchar *query, const gchar *category, gint page, gint size, gboolean *has_next, GError **error) {
    gchar *encoded = url_encode(query);
    gchar *encoded_category = category && *category ? url_encode(category) : NULL;
    gchar *endpoint = join_url(base_url, "/v1/search");
    gchar *url = encoded_category
        ? g_strdup_printf("%s?q=%s&category=%s&page=%d&size=%d", endpoint, encoded, encoded_category, page, size)
        : g_strdup_printf("%s?q=%s&page=%d&size=%d", endpoint, encoded, page, size);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    GPtrArray *books = body ? parse_books(body) : NULL;
    if (has_next) *has_next = body && strstr(body, "\"has_next\":true") != NULL;
    g_free(encoded); g_free(encoded_category); g_free(endpoint); g_free(url); g_free(body);
    return books;
}

GPtrArray *bookrelay_api_subcategories(const gchar *base_url, const gchar *token, const gchar *category, GError **error) {
    gchar *encoded = url_encode(category);
    gchar *endpoint = join_url(base_url, "/v1/subcategories");
    gchar *url = g_strdup_printf("%s?category=%s", endpoint, encoded);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    GPtrArray *items = body ? g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_category_free) : NULL;
    const gchar *cursor = body;
    while (items && (cursor = strstr(cursor, "\"id\":\"")) != NULL) {
        const gchar *end = strchr(cursor, '}');
        BookRelayCategory *item;
        if (!end) break;
        item = g_new0(BookRelayCategory, 1);
        item->id = json_string(cursor, "id");
        item->title = json_string(cursor, "title");
        g_ptr_array_add(items, item);
        cursor = end + 1;
    }
    g_free(encoded); g_free(endpoint); g_free(url); g_free(body);
    return items;
}

GPtrArray *bookrelay_api_catalog_books(const gchar *base_url, const gchar *token, const gchar *category, const gchar *subcategory, gint page, gint size, gboolean *has_next, GError **error) {
    gchar *encoded_category = url_encode(category);
    gchar *encoded_subcategory = url_encode(subcategory);
    gchar *endpoint = join_url(base_url, "/v1/catalog/books");
    gchar *url = g_strdup_printf("%s?category=%s&subcategory=%s&page=%d&size=%d", endpoint, encoded_category, encoded_subcategory, page, size);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    GPtrArray *books = body ? parse_books(body) : NULL;
    if (has_next) *has_next = body && strstr(body, "\"has_next\":true") != NULL;
    g_free(encoded_category); g_free(encoded_subcategory); g_free(endpoint); g_free(url); g_free(body);
    return books;
}

GPtrArray *bookrelay_api_categories(const gchar *base_url, const gchar *token, GError **error) {
    gchar *url = join_url(base_url, "/v1/categories");
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    GPtrArray *items = body ? g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_category_free) : NULL;
    const gchar *cursor = body;
    while (items && (cursor = strstr(cursor, "\"id\":\"")) != NULL) {
        const gchar *end = strchr(cursor, '}');
        BookRelayCategory *category;
        if (!end) break;
        category = g_new0(BookRelayCategory, 1);
        category->id = json_string(cursor, "id");
        category->title = json_string(cursor, "title");
        g_ptr_array_add(items, category);
        cursor = end + 1;
    }
    g_free(url); g_free(body);
    return items;
}

BookRelayBook *bookrelay_api_book(const gchar *base_url, const gchar *token, const gchar *book_id, GError **error) {
    gchar *path = g_strdup_printf("/v1/books/%s", book_id);
    gchar *url = join_url(base_url, path);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    BookRelayBook *book = NULL;
    if (body) {
        book = g_new0(BookRelayBook, 1);
        book->id = json_string(body, "id");
        book->title = json_string(body, "title");
        book->author = json_string(body, "author");
        book->cover_url = json_string(body, "cover_url");
        book->description = json_string(body, "description");
        book->translator = json_string(body, "translator");
        book->year = json_int(body, "year");
    }
    g_free(path); g_free(url); g_free(body);
    return book;
}

BookRelayClaim *bookrelay_api_pair_claim(const gchar *base_url, const gchar *code, GError **error) {
    gchar *url = join_url(base_url, "/v1/pair/claim");
    gchar *escaped_code = json_escape(code);
    gchar *body = g_strdup_printf("{\"code\":\"%s\"}", escaped_code);
    long status;
    gchar *response = request("POST", url, NULL, body, &status, error);
    BookRelayClaim *claim = NULL;
    if (response) {
        claim = g_new0(BookRelayClaim, 1);
        claim->token = json_string(response, "token");
        claim->kindle_email = json_string(response, "kindle_email");
        if (!claim->token || !*claim->token) {
            g_set_error(error, API_ERROR, 4, "relay ответил HTTP %ld, но не вернул токен устройства; проверьте адрес сервера и версию relay", status);
            bookrelay_claim_free(claim);
            claim = NULL;
        }
    }
    g_free(url); g_free(escaped_code); g_free(body); g_free(response);
    return claim;
}

gchar *bookrelay_api_update_email(const gchar *base_url, const gchar *token, const gchar *email, GError **error) {
    gchar *url = join_url(base_url, "/v1/devices/me");
    gchar *escaped_email = json_escape(email);
    gchar *body = g_strdup_printf("{\"kindle_email\":\"%s\"}", escaped_email);
    long status;
    gchar *response = request("PUT", url, token, body, &status, error);
    gchar *updated = response ? json_string(response, "kindle_email") : NULL;
    g_free(url); g_free(escaped_email); g_free(body); g_free(response);
    return updated;
}

gchar *bookrelay_api_send(const gchar *base_url, const gchar *token, const gchar *book_id, const gchar *title, GError **error) {
    gchar *url = join_url(base_url, "/v1/deliveries");
    gchar *escaped_book_id = json_escape(book_id);
    gchar *escaped_title = json_escape(title);
    gchar *body = g_strdup_printf("{\"book_id\":\"%s\",\"title\":\"%s\"}", escaped_book_id, escaped_title);
    long status;
    gchar *response = request("POST", url, token, body, &status, error);
    gchar *job_id = response ? json_string(response, "id") : NULL;
    g_free(url); g_free(escaped_book_id); g_free(escaped_title); g_free(body); g_free(response);
    return job_id;
}

gchar *bookrelay_api_delivery_status(const gchar *base_url, const gchar *token, const gchar *job_id, GError **error) {
    gchar *path = g_strdup_printf("/v1/deliveries/%s", job_id);
    gchar *url = join_url(base_url, path);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    gchar *state = body ? json_string(body, "status") : NULL;
    g_free(path); g_free(url); g_free(body);
    return state;
}

gboolean bookrelay_api_download(const gchar *base_url, const gchar *token, const gchar *url, GByteArray **payload, GError **error) {
    GByteArray *bytes = NULL;
    long status;
    gchar *full_url = g_str_has_prefix(url, "/v1/books/") ? join_url(base_url, url) : g_strdup(url);
    gboolean ok = request_bytes("GET", full_url, g_str_has_prefix(url, "/v1/books/") ? token : NULL,
                                NULL, MAX_COVER_RESPONSE_BYTES, &bytes, &status, error);
    g_free(full_url);
    if (!ok) return FALSE;
    *payload = bytes;
    return TRUE;
}

void bookrelay_book_free(BookRelayBook *book) {
    if (!book) return;
    g_free(book->id); g_free(book->title); g_free(book->author); g_free(book->cover_url);
    g_free(book->description); g_free(book->translator); g_free(book);
}

BookRelayBook *bookrelay_book_copy(const BookRelayBook *book) {
    BookRelayBook *copy = g_new0(BookRelayBook, 1);
    copy->id = g_strdup(book->id);
    copy->title = g_strdup(book->title);
    copy->author = g_strdup(book->author);
    copy->cover_url = g_strdup(book->cover_url);
    copy->description = g_strdup(book->description);
    copy->translator = g_strdup(book->translator);
    copy->year = book->year;
    return copy;
}

void bookrelay_claim_free(BookRelayClaim *claim) {
    if (!claim) return;
    g_free(claim->token); g_free(claim->kindle_email); g_free(claim);
}

void bookrelay_category_free(BookRelayCategory *category) {
    if (!category) return;
    g_free(category->id);
    g_free(category->title);
    g_free(category);
}
