#include "../src/api.h"

#include <glib.h>
#include <stdio.h>

int main(int argc, char **argv) {
    GError *error = NULL;
    gchar *email;
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
