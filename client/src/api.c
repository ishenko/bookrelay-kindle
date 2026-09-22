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
    if (g_strcmp0(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
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
        g_set_error(error, API_ERROR, (gint)*status, "relay returned HTTP %ld", *status);
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
    gchar *needle = g_strdup_printf("\"%s\":\"", key);
    const gchar *start = strstr(object, needle);
    const gchar *cursor;
    GString *raw;
    gchar *result;
    g_free(needle);
    if (!start) return g_strdup("");
    cursor = start + strlen(key) + 4;
    raw = g_string_new(NULL);
    while (*cursor) {
        if (*cursor == '"' && (raw->len == 0 || raw->str[raw->len - 1] != '\\')) break;
        g_string_append_c(raw, *cursor++);
    }
    result = json_unescape(raw->str, raw->len);
    g_string_free(raw, TRUE);
    return result;
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
        g_ptr_array_add(books, book);
        g_free(object);
        cursor = end + 1;
    }
    return books;
}

static gchar *join_url(const gchar *base, const gchar *path) {
    return g_strdup_printf("%s/%s", base, path[0] == '/' ? path + 1 : path);
}

GPtrArray *bookrelay_api_search(const gchar *base_url, const gchar *token, const gchar *query, gint page, GError **error) {
    gchar *encoded = url_encode(query);
    gchar *url = g_strdup_printf("%s/v1/search?q=%s&page=%d", base_url, encoded, page);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    GPtrArray *books = body ? parse_books(body) : NULL;
    g_free(encoded); g_free(url); g_free(body);
    return books;
}

GPtrArray *bookrelay_api_categories(const gchar *base_url, const gchar *token, GError **error) {
    gchar *url = join_url(base_url, "/v1/categories");
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    GPtrArray *items = body ? g_ptr_array_new_with_free_func(g_free) : NULL;
    const gchar *cursor = body;
    while (items && (cursor = strstr(cursor, "\"title\":\"")) != NULL) {
        g_ptr_array_add(items, json_string(cursor, "title"));
        cursor += 8;
    }
    g_free(url); g_free(body);
    return items;
}

BookRelayBook *bookrelay_api_book(const gchar *base_url, const gchar *token, const gchar *book_id, GError **error) {
    gchar *url = g_strdup_printf("%s/v1/books/%s", base_url, book_id);
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
    }
    g_free(url); g_free(body);
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
    }
    g_free(url); g_free(escaped_code); g_free(body); g_free(response);
    return claim;
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
    gchar *url = g_strdup_printf("%s/v1/deliveries/%s", base_url, job_id);
    long status;
    gchar *body = request("GET", url, token, NULL, &status, error);
    gchar *state = body ? json_string(body, "status") : NULL;
    g_free(url); g_free(body);
    return state;
}

gboolean bookrelay_api_download(const gchar *url, GByteArray **payload, GError **error) {
    GByteArray *bytes = NULL;
    long status;
    if (!request_bytes("GET", url, NULL, NULL, MAX_COVER_RESPONSE_BYTES, &bytes, &status, error)) return FALSE;
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
