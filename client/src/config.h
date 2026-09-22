#ifndef BOOKRELAY_CONFIG_H
#define BOOKRELAY_CONFIG_H

#include <glib.h>

typedef struct {
    gchar *relay_url;
    gchar *device_id;
    gchar *kindle_email;
    gchar *token;
    gboolean auto_download;
} BookRelayConfig;

BookRelayConfig *bookrelay_config_load(const gchar *path);
gboolean bookrelay_config_save(const BookRelayConfig *config, const gchar *path, GError **error);
void bookrelay_config_free(BookRelayConfig *config);

#endif
