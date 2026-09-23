#include "../src/api.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    GError *error = NULL;
    gchar *email;
    if (argc == 3) {
        BookRelayClaim *claim;
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
