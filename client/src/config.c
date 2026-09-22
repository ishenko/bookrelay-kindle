#include "config.h"

#include <glib/gstdio.h>

BookRelayConfig *bookrelay_config_load(const gchar *path) {
    GKeyFile *key_file = g_key_file_new();
    BookRelayConfig *config = g_new0(BookRelayConfig, 1);
    GError *error = NULL;

    config->relay_url = g_strdup("https://relay.example.invalid");
    config->device_id = g_strdup("kindle");
    config->kindle_email = g_strdup("");
    config->token = g_strdup("");
    config->auto_download = FALSE;

    if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
        gchar *value = g_key_file_get_string(key_file, "relay", "url", NULL);
        if (value) { g_free(config->relay_url); config->relay_url = value; }
        value = g_key_file_get_string(key_file, "device", "id", NULL);
        if (value) { g_free(config->device_id); config->device_id = value; }
        value = g_key_file_get_string(key_file, "device", "kindle_email", NULL);
        if (value) { g_free(config->kindle_email); config->kindle_email = value; }
        value = g_key_file_get_string(key_file, "device", "token", NULL);
        if (value) { g_free(config->token); config->token = value; }
        config->auto_download = g_key_file_get_boolean(key_file, "device", "auto_download", NULL);
    }
    g_clear_error(&error);
    g_key_file_free(key_file);
    return config;
}

gboolean bookrelay_config_save(const BookRelayConfig *config, const gchar *path, GError **error) {
    GKeyFile *key_file = g_key_file_new();
    gchar *contents;
    gsize length;
    gchar *directory = g_path_get_dirname(path);
    gboolean ok;

    g_mkdir_with_parents(directory, 0700);
    g_free(directory);
    g_key_file_set_string(key_file, "relay", "url", config->relay_url);
    g_key_file_set_string(key_file, "device", "id", config->device_id);
    g_key_file_set_string(key_file, "device", "kindle_email", config->kindle_email);
    g_key_file_set_string(key_file, "device", "token", config->token);
    g_key_file_set_boolean(key_file, "device", "auto_download", config->auto_download);
    contents = g_key_file_to_data(key_file, &length, error);
    ok = contents && g_file_set_contents(path, contents, (gssize)length, error);
    g_free(contents);
    g_key_file_free(key_file);
    return ok;
}

void bookrelay_config_free(BookRelayConfig *config) {
    if (!config) return;
    g_free(config->relay_url);
    g_free(config->device_id);
    g_free(config->kindle_email);
    g_free(config->token);
    g_free(config);
}
