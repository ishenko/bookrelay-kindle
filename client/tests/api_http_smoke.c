#include "../src/api.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    GError *error = NULL;
    gchar *email;
    if (argc == 3) {
        BookRelayClaim *claim;
        if (strcmp(argv[2], "catalog-json") == 0) {
            gboolean has_next = FALSE;
            GPtrArray *books = bookrelay_api_search(argv[1], "test-device-token", "книга", NULL, 1, 12, &has_next, &error);
            BookRelayBook *first, *second;
            if (!books || books->len != 2 || !has_next) {
                fputs(error ? error->message : "search books were lost", stderr);
                return 1;
            }
            first = g_ptr_array_index(books, 0);
            second = g_ptr_array_index(books, 1);
            if (g_strcmp0(first->title, "Книга } с кавычками") != 0 ||
                g_strcmp0(first->description, "Текст } и вложенный объект") != 0 ||
                g_strcmp0(first->cover_url, "/v1/books/1/cover?path=cover.jpg") != 0 ||
                g_strcmp0(second->title, "Название с обратным слешем \\") != 0 ||
                g_strcmp0(second->author, "Автор") != 0 || second->year != 2022) {
                fputs("search book fields were parsed incorrectly", stderr);
                return 1;
            }
            g_ptr_array_free(books, TRUE);
            return 0;
        }
        if (strcmp(argv[2], "cover") == 0) {
            GByteArray *bytes = NULL;
            if (!bookrelay_api_download(argv[1], "test-device-token",
                    "/v1/books/451198/cover?path=%2Fi%2F98%2F451198%2Fcover.jpg", &bytes, &error) ||
                !bytes || bytes->len != 4 || memcmp(bytes->data, "JPEG", 4) != 0) {
                fputs(error ? error->message : "cover response could not be downloaded", stderr);
                return 1;
            }
            g_byte_array_free(bytes, TRUE);
            return 0;
        }
        if (strcmp(argv[2], "invalid-response") == 0) {
            claim = bookrelay_api_pair_claim(argv[1], "abcdef12", &error);
            if (claim || !error || !strstr(error->message, "не вернул токен")) return 1;
            g_clear_error(&error);
            return 0;
        }
        if (strcmp(argv[2], "pair") != 0) return 2;
        claim = bookrelay_api_pair_claim(argv[1], "abcdef12", &error);
        if (!claim || g_strcmp0(claim->token, "mock-device-token") != 0 ||
            g_strcmp0(claim->kindle_email, "reader@kindle.com") != 0) {
            fputs(error ? error->message : "claim response could not be parsed", stderr);
            return 1;
        }
        bookrelay_claim_free(claim);
        return 0;
    }
    if (argc != 2) return 2;
    email = bookrelay_api_update_email(argv[1], "test-device-token", "reader@kindle.com", &error);
    if (!email || g_strcmp0(email, "reader@kindle.com") != 0) {
        fputs(error ? error->message : "unexpected email response", stderr);
        fputc(10, stderr);
        g_clear_error(&error);
        g_free(email);
        return 1;
    }
    g_free(email);
    return 0;
}
