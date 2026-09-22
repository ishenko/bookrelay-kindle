#include "api.h"
#include "config.h"

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <curl/curl.h>

#define APP_NAME "BookRelay Kindle"

typedef struct {
    BookRelayConfig *config;
    gchar *config_path;
    GtkWidget *window;
    GtkWidget *query;
    GtkWidget *categories;
    GtkWidget *results;
    GtkWidget *status;
    GtkWidget *previous_page;
    GtkWidget *next_page;
    guint page;
} App;

typedef struct {
    App *app;
    BookRelayBook *book;
} BookRow;

typedef struct {
    App *app;
    gchar *job_id;
    guint attempts;
} DeliveryPoll;

static void set_status(App *app, const gchar *message);
static void show_error(App *app, const gchar *prefix, GError *error);

static void book_row_free(BookRow *row) {
    if (!row) return;
    bookrelay_book_free(row->book);
    g_free(row);
}

static gboolean poll_delivery(gpointer userdata) {
    DeliveryPoll *poll = userdata;
    GError *error = NULL;
    gchar *state = bookrelay_api_delivery_status(poll->app->config->relay_url, poll->app->config->token, poll->job_id, &error);
    poll->attempts++;
    if (!state) {
        if (poll->attempts >= 20) {
            show_error(poll->app, "Не удалось получить статус доставки", error);
            g_free(poll->job_id);
            g_free(poll);
            return G_SOURCE_REMOVE;
        }
        g_clear_error(&error);
        return G_SOURCE_CONTINUE;
    }
    if (g_strcmp0(state, "sent") == 0) {
        set_status(poll->app, "Relay отправил EPUB; ожидайте доставку Amazon");
    } else if (g_strcmp0(state, "failed") == 0) {
        set_status(poll->app, "Relay не смог отправить EPUB");
    } else if (poll->attempts >= 20) {
        set_status(poll->app, "Задание всё ещё выполняется; проверьте статус relay позже");
    } else {
        g_free(state);
        return G_SOURCE_CONTINUE;
    }
    g_free(state);
    g_free(poll->job_id);
    g_free(poll);
    return G_SOURCE_REMOVE;
}

static void set_status(App *app, const gchar *message) {
    gtk_label_set_text(GTK_LABEL(app->status), message);
}

static void show_error(App *app, const gchar *prefix, GError *error) {
    gchar *message = g_strdup_printf("%s: %s", prefix, error ? error->message : "unknown error");
    set_status(app, message);
    g_free(message);
    g_clear_error(&error);
}

static gchar *cover_cache_path(const gchar *book_id) {
    const gchar *base = g_get_user_data_dir();
    gchar *directory = g_build_filename(base, "bookrelay", "covers", NULL);
    gchar *path;
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, book_id, NULL);
    g_free(directory);
    return path;
}

static GtkWidget *make_cover(App *app, BookRelayBook *book) {
    gchar *path = cover_cache_path(book->id);
    GdkPixbuf *pixbuf = NULL;
    GtkWidget *image;
    if (g_file_test(path, G_FILE_TEST_EXISTS)) pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 90, 130, TRUE, NULL);
    if (!pixbuf && book->cover_url && *book->cover_url) {
        GError *error = NULL;
        GBytes *bytes = NULL;
        if (bookrelay_api_download(book->cover_url, &bytes, &error)) {
            g_file_set_contents(path, g_bytes_get_data(bytes, NULL), (gssize)g_bytes_get_size(bytes), NULL);
            pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 90, 130, TRUE, NULL);
            g_bytes_unref(bytes);
        }
        g_clear_error(&error);
    }
    image = pixbuf ? gtk_image_new_from_pixbuf(pixbuf) : gtk_image_new_from_icon_name("text-x-generic", GTK_ICON_SIZE_DIALOG);
    if (pixbuf) g_object_unref(pixbuf);
    g_free(path);
    return image;
}

static void send_book(App *app, BookRelayBook *book) {
    GError *error = NULL;
    gchar *job_id;
    if (!app->config->token || !*app->config->token) {
        set_status(app, "Сначала выполните pairing в настройках");
        return;
    }
    set_status(app, "Отправка книги на Kindle…");
    job_id = bookrelay_api_send(app->config->relay_url, app->config->token, book->id, book->title, &error);
    if (!job_id) { show_error(app, "Не удалось создать задание", error); return; }
    set_status(app, "Задание создано, relay готовит EPUB…");
    DeliveryPoll *poll = g_new0(DeliveryPoll, 1);
    poll->app = app;
    poll->job_id = job_id;
    g_timeout_add_seconds(3, poll_delivery, poll);
}

static void show_details(GtkButton *button, gpointer userdata) {
    BookRow *row = userdata;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(row->book->title, GTK_WINDOW(row->app->window), GTK_DIALOG_MODAL, "Закрыть", GTK_RESPONSE_CLOSE, "Скачать на Kindle", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new(NULL);
    gchar *text = g_strdup_printf("%s\n\nАвтор: %s\n\n%s", row->book->title, row->book->author, row->book->description && *row->book->description ? row->book->description : "Описание отсутствует.");
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(content), label, TRUE, TRUE, 12);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) send_book(row->app, row->book);
    gtk_widget_destroy(dialog);
    g_free(text);
}

static void clear_results(App *app) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->results));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
}

static void render_books(App *app, GPtrArray *books) {
    guint i;
    clear_results(app);
    for (i = 0; i < books->len; i++) {
        BookRelayBook *book = g_ptr_array_index(books, i);
        GtkWidget *row = gtk_hbox_new(FALSE, 10);
        GtkWidget *text_box = gtk_vbox_new(FALSE, 4);
        GtkWidget *title = gtk_label_new(book->title);
        GtkWidget *author = gtk_label_new(book->author);
        GtkWidget *button = gtk_button_new_with_label("Подробнее");
        BookRow *data = g_new0(BookRow, 1);
        data->app = app;
        data->book = bookrelay_book_copy(book);
        g_object_set_data_full(G_OBJECT(button), "book-row", data, (GDestroyNotify)book_row_free);
        g_signal_connect(button, "clicked", G_CALLBACK(show_details), data);
        gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
        gtk_misc_set_alignment(GTK_MISC(author), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(row), make_cover(app, book), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_box), author, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(text_box), button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), text_box, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(app->results), row, FALSE, FALSE, 8);
    }
    gtk_widget_show_all(app->results);
}

static void search_page(App *app, guint page) {
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(app->query));
    GError *error = NULL;
    GPtrArray *books;

    if (!query || !*query) return;
    set_status(app, "Ищем книги…");
    books = bookrelay_api_search(app->config->relay_url, app->config->token, query, page, &error);
    if (!books) { show_error(app, "Поиск не выполнен", error); return; }
    if (books->len == 0 && page > 1) {
        gtk_widget_set_sensitive(app->next_page, FALSE);
        set_status(app, "Это последняя страница");
        g_ptr_array_free(books, TRUE);
        return;
    }
    app->page = page;
    render_books(app, books);
    gtk_widget_set_sensitive(app->previous_page, page > 1);
    gtk_widget_set_sensitive(app->next_page, books->len > 0);
    set_status(app, "Поиск завершён");
    g_ptr_array_free(books, TRUE);
}

static void search_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    search_page(app, 1);
}

static void previous_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (app->page > 1) search_page(app, app->page - 1);
}

static void next_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    search_page(app, app->page + 1);
}

static gboolean poll_pairing(gpointer userdata) {
    App *app = userdata;
    const gchar *code = g_object_get_data(G_OBJECT(app->window), "pairing-code");
    GError *error = NULL;
    gchar *email = NULL;
    gchar *token;
    if (!code) return G_SOURCE_REMOVE;
    token = bookrelay_api_pair_status(app->config->relay_url, code, &email, &error);
    if (token) {
        g_free(app->config->token); app->config->token = token;
        if (email) { g_free(app->config->kindle_email); app->config->kindle_email = email; }
        bookrelay_config_save(app->config, app->config_path, NULL);
        set_status(app, "Kindle привязан");
        g_object_set_data(G_OBJECT(app->window), "pairing-code", NULL);
        return G_SOURCE_REMOVE;
    }
    g_clear_error(&error);
    return G_SOURCE_CONTINUE;
}

static void pair_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GError *error = NULL;
    gchar *message;
    BookRelayPairing *pairing = bookrelay_api_start_pairing(app->config->relay_url, app->config->device_id, &error);
    if (!pairing) { show_error(app, "Pairing не запущен", error); return; }
    g_object_set_data_full(G_OBJECT(app->window), "pairing-code", g_strdup(pairing->code), g_free);
    message = g_strdup_printf("Код pairing: %s\nОткройте relay /pair на компьютере и введите этот код вместе с Kindle Email", pairing->code);
    set_status(app, message);
    g_free(message);
    g_timeout_add_seconds(3, poll_pairing, app);
    bookrelay_pairing_free(pairing);
}

static void settings_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Настройки", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "Отмена", GTK_RESPONSE_CANCEL, "Сохранить", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    GtkWidget *relay = gtk_entry_new();
    GtkWidget *device = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(relay), app->config->relay_url);
    gtk_entry_set_text(GTK_ENTRY(device), app->config->device_id);
    gtk_table_attach_defaults(GTK_TABLE(table), gtk_label_new("Relay URL"), 0, 1, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(table), relay, 1, 2, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(table), gtk_label_new("Device ID"), 0, 1, 1, 2);
    gtk_table_attach_defaults(GTK_TABLE(table), device, 1, 2, 1, 2);
    gtk_container_set_border_width(GTK_CONTAINER(table), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), table, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_free(app->config->relay_url); app->config->relay_url = g_strdup(gtk_entry_get_text(GTK_ENTRY(relay)));
        g_free(app->config->device_id); app->config->device_id = g_strdup(gtk_entry_get_text(GTK_ENTRY(device)));
        bookrelay_config_save(app->config, app->config_path, NULL);
        set_status(app, "Настройки сохранены");
    }
    gtk_widget_destroy(dialog);
}

static void exit_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Выйти из BookRelay Kindle?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Все настройки и pairing останутся сохранены.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Отмена", GTK_RESPONSE_CANCEL, "Выйти", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        gtk_main_quit();
        return;
    }
    gtk_widget_destroy(dialog);
}

static void build_ui(App *app) {
    GtkWidget *root = gtk_vbox_new(FALSE, 8);
    GtkWidget *toolbar = gtk_hbox_new(FALSE, 6);
    GtkWidget *search_button = gtk_button_new_with_label("Искать");
    GtkWidget *pair_button = gtk_button_new_with_label("Pairing");
    GtkWidget *settings_button = gtk_button_new_with_label("Настройки");
    GtkWidget *exit_button = gtk_button_new_with_label("Выйти");
    GtkWidget *navigation = gtk_hbox_new(FALSE, 6);
    GtkWidget *previous_page = gtk_button_new_with_label("Назад");
    GtkWidget *next_page = gtk_button_new_with_label("Дальше");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    app->query = gtk_entry_new();
    app->categories = gtk_combo_box_new_text();
    app->results = gtk_vbox_new(FALSE, 4);
    app->status = gtk_label_new("Готово");
    app->previous_page = previous_page;
    app->next_page = next_page;
    app->page = 1;
    gtk_window_set_title(GTK_WINDOW(app->window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 600, 800);
    gtk_entry_set_text(GTK_ENTRY(app->query), "");
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->categories), "Все категории");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->categories), 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->query, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->categories, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), pair_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), settings_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), exit_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(scroll), app->results);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), previous_page, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), next_page, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), navigation, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->status, FALSE, FALSE, 8);
    gtk_container_add(GTK_CONTAINER(app->window), root);
    g_signal_connect(search_button, "clicked", G_CALLBACK(search_clicked), app);
    g_signal_connect(pair_button, "clicked", G_CALLBACK(pair_clicked), app);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(settings_clicked), app);
    g_signal_connect(exit_button, "clicked", G_CALLBACK(exit_clicked), app);
    g_signal_connect(previous_page, "clicked", G_CALLBACK(previous_page_clicked), app);
    g_signal_connect(next_page, "clicked", G_CALLBACK(next_page_clicked), app);
    gtk_widget_set_sensitive(previous_page, FALSE);
    gtk_widget_set_sensitive(next_page, FALSE);
    gtk_widget_show_all(app->window);
    if (app->config->token && *app->config->token) {
        GError *error = NULL;
        GPtrArray *categories = bookrelay_api_categories(app->config->relay_url, app->config->token, &error);
        if (categories) {
            guint i;
            for (i = 0; i < categories->len; i++) gtk_combo_box_append_text(GTK_COMBO_BOX(app->categories), g_ptr_array_index(categories, i));
            g_ptr_array_free(categories, TRUE);
        }
        g_clear_error(&error);
    }
}

int main(int argc, char **argv) {
    App app = {0};
    const gchar *home = g_get_user_data_dir();
    app.config_path = g_build_filename(home, "bookrelay", "config.ini", NULL);
    app.config = bookrelay_config_load(app.config_path);
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    build_ui(&app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(app.window);
    gtk_main();
    bookrelay_config_free(app.config);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
