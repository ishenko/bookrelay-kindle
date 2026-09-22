#include "api.h"
#include "config.h"

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <curl/curl.h>
#include <sys/stat.h>

#define APP_NAME "BookRelay Kindle"
#define MAX_COVER_CACHE_BYTES (64u * 1024u * 1024u)

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
    guint active_tasks;
} App;

typedef struct {
    App *app;
    BookRelayBook *book;
} BookRow;

typedef struct {
    App *app;
    gchar *job_id;
    guint attempts;
    guint source_id;
    gboolean request_pending;
} DeliveryPoll;

typedef enum {
    TASK_SEARCH,
    TASK_CATEGORIES,
    TASK_PAIR_CLAIM,
    TASK_SEND,
    TASK_DELIVERY_STATUS,
    TASK_COVER
} TaskKind;

typedef struct {
    App *app;
    TaskKind kind;
    gchar *base_url;
    gchar *token;
    gchar *query;
    gchar *code;
    gchar *book_id;
    gchar *title;
    gchar *job_id;
    gchar *url;
    guint page;
    GtkWidget *image;
    DeliveryPoll *delivery_poll;
    GPtrArray *books;
    GPtrArray *categories;
    BookRelayClaim *claim;
    gchar *state;
    GByteArray *cover_bytes;
    GError *error;
} AsyncTask;

static void set_status(App *app, const gchar *message);
static void show_error(App *app, const gchar *prefix, GError *error);
static gboolean async_task_complete(gpointer userdata);
static void show_details(GtkButton *button, gpointer userdata);

static void book_row_free(BookRow *row) {
    if (!row) return;
    bookrelay_book_free(row->book);
    g_free(row);
}

static void async_task_free(AsyncTask *task) {
    if (!task) return;
    g_free(task->base_url);
    g_free(task->token);
    g_free(task->query);
    g_free(task->code);
    g_free(task->book_id);
    g_free(task->title);
    g_free(task->job_id);
    g_free(task->url);
    if (task->image) g_object_unref(task->image);
    if (task->books) g_ptr_array_free(task->books, TRUE);
    if (task->categories) g_ptr_array_free(task->categories, TRUE);
    bookrelay_claim_free(task->claim);
    g_free(task->state);
    if (task->cover_bytes) g_byte_array_free(task->cover_bytes, TRUE);
    if (task->error) g_error_free(task->error);
    g_free(task);
}

static AsyncTask *async_task_new(App *app, TaskKind kind) {
    AsyncTask *task = g_new0(AsyncTask, 1);
    task->app = app;
    task->kind = kind;
    app->active_tasks++;
    return task;
}

static gpointer async_task_worker(gpointer userdata) {
    AsyncTask *task = userdata;
    switch (task->kind) {
        case TASK_SEARCH:
            task->books = bookrelay_api_search(task->base_url, task->token, task->query, (gint)task->page, &task->error);
            break;
        case TASK_CATEGORIES:
            task->categories = bookrelay_api_categories(task->base_url, task->token, &task->error);
            break;
        case TASK_PAIR_CLAIM:
            task->claim = bookrelay_api_pair_claim(task->base_url, task->code, &task->error);
            break;
        case TASK_SEND:
            task->job_id = bookrelay_api_send(task->base_url, task->token, task->book_id, task->title, &task->error);
            break;
        case TASK_DELIVERY_STATUS:
            task->state = bookrelay_api_delivery_status(task->base_url, task->token, task->job_id, &task->error);
            break;
        case TASK_COVER:
            bookrelay_api_download(task->url, &task->cover_bytes, &task->error);
            break;
    }
    g_idle_add(async_task_complete, task);
    return NULL;
}

static gboolean start_async_task(AsyncTask *task) {
    GThread *thread;
#if GLIB_CHECK_VERSION(2, 32, 0)
    thread = g_thread_new("bookrelay-network", async_task_worker, task);
#else
    GError *thread_error = NULL;
    thread = g_thread_create(async_task_worker, task, FALSE, &thread_error);
    if (thread_error) g_error_free(thread_error);
#endif
    if (!thread) {
        task->app->active_tasks--;
        async_task_free(task);
        return FALSE;
    }
#if GLIB_CHECK_VERSION(2, 32, 0)
    g_thread_unref(thread);
#endif
    return TRUE;
}

static void copy_common_task_fields(AsyncTask *task, App *app) {
    task->base_url = g_strdup(app->config->relay_url);
    task->token = g_strdup(app->config->token);
}

static void set_status(App *app, const gchar *message) {
    gtk_label_set_text(GTK_LABEL(app->status), message);
}

static void show_error(App *app, const gchar *prefix, GError *error) {
    gchar *message = g_strdup_printf("%s: %s", prefix, error ? error->message : "unknown error");
    set_status(app, message);
    g_free(message);
}

static gboolean cache_has_room(const gchar *directory, const gchar *path, gsize incoming) {
    GDir *dir = g_dir_open(directory, 0, NULL);
    const gchar *name;
    guint64 total = 0;
    struct stat existing;
    gboolean has_existing = g_stat(path, &existing) == 0 && S_ISREG(existing.st_mode);
    guint64 existing_size = has_existing ? (guint64)existing.st_size : 0;
    if (!dir) return incoming <= MAX_COVER_CACHE_BYTES;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *entry = g_build_filename(directory, name, NULL);
        struct stat info;
        if (g_stat(entry, &info) == 0 && S_ISREG(info.st_mode)) total += (guint64)info.st_size;
        g_free(entry);
    }
    g_dir_close(dir);
    if (total >= existing_size) total -= existing_size;
    return incoming <= MAX_COVER_CACHE_BYTES && total <= MAX_COVER_CACHE_BYTES - incoming;
}

static gchar *cover_cache_path(const gchar *book_id) {
    const gchar *base = g_get_user_data_dir();
    gchar *directory = g_build_filename(base, "bookrelay", "covers", NULL);
    gchar *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, book_id ? book_id : "", -1);
    gchar *path;
    g_mkdir_with_parents(directory, 0700);
    path = g_build_filename(directory, checksum, NULL);
    g_free(checksum);
    g_free(directory);
    return path;
}

static GdkPixbuf *pixbuf_from_bytes(GByteArray *bytes) {
    GdkPixbufLoader *loader;
    GdkPixbuf *pixbuf;
    gsize length = bytes->len;
    const guint8 *data = bytes->data;
    loader = gdk_pixbuf_loader_new();
    if (!gdk_pixbuf_loader_write(loader, data, length, NULL) || !gdk_pixbuf_loader_close(loader, NULL)) {
        g_object_unref(loader);
        return NULL;
    }
    pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf) g_object_ref(pixbuf);
    g_object_unref(loader);
    return pixbuf;
}

static GtkWidget *make_cover(App *app, BookRelayBook *book) {
    gchar *path = cover_cache_path(book->id);
    GdkPixbuf *pixbuf = NULL;
    GtkWidget *image;
    if (g_file_test(path, G_FILE_TEST_EXISTS)) pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 90, 130, TRUE, NULL);
    image = pixbuf ? gtk_image_new_from_pixbuf(pixbuf) : gtk_image_new_from_icon_name("text-x-generic", GTK_ICON_SIZE_DIALOG);
    if (pixbuf) g_object_unref(pixbuf);
    if (!pixbuf && book->cover_url && *book->cover_url) {
        AsyncTask *task = async_task_new(app, TASK_COVER);
        copy_common_task_fields(task, app);
        task->book_id = g_strdup(book->id);
        task->url = g_strdup(book->cover_url);
        task->image = g_object_ref(image);
        start_async_task(task);
    }
    g_free(path);
    return image;
}

static void render_books(App *app, GPtrArray *books) {
    guint i;
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->results));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
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
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        AsyncTask *task;
        if (!row->app->config->token || !*row->app->config->token) {
            set_status(row->app, "Сначала выполните pairing в настройках");
        } else {
            set_status(row->app, "Отправляем книгу на Kindle…");
            task = async_task_new(row->app, TASK_SEND);
            copy_common_task_fields(task, row->app);
            task->book_id = g_strdup(row->book->id);
            task->title = g_strdup(row->book->title);
            start_async_task(task);
        }
    }
    gtk_widget_destroy(dialog);
    g_free(text);
}

static void search_page(App *app, guint page) {
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(app->query));
    AsyncTask *task;
    if (!query || !*query) return;
    set_status(app, "Ищем книги…");
    task = async_task_new(app, TASK_SEARCH);
    copy_common_task_fields(task, app);
    task->query = g_strdup(query);
    task->page = page;
    start_async_task(task);
}

static void search_clicked(GtkButton *button, gpointer userdata) {
    search_page((App *)userdata, 1);
}

static void previous_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (app->page > 1) search_page(app, app->page - 1);
}

static void next_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    search_page(app, app->page + 1);
}

static void pair_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Подключение Kindle", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "Отмена", GTK_RESPONSE_CANCEL, "Подключить", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    GtkWidget *relay = gtk_entry_new();
    GtkWidget *code = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(relay), app->config->relay_url);
    gtk_entry_set_max_length(GTK_ENTRY(code), 32);
    gtk_entry_set_activates_default(GTK_ENTRY(code), TRUE);
    gtk_table_attach_defaults(GTK_TABLE(table), gtk_label_new("Relay URL"), 0, 1, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(table), relay, 1, 2, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(table), gtk_label_new("Одноразовый код"), 0, 1, 1, 2);
    gtk_table_attach_defaults(GTK_TABLE(table), code, 1, 2, 1, 2);
    gtk_container_set_border_width(GTK_CONTAINER(table), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), table, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *relay_url = gtk_entry_get_text(GTK_ENTRY(relay));
        const gchar *pairing_code = gtk_entry_get_text(GTK_ENTRY(code));
        if (!relay_url || !*relay_url || !pairing_code || !*pairing_code) {
            set_status(app, "Введите Relay URL и одноразовый код");
        } else {
            AsyncTask *task = async_task_new(app, TASK_PAIR_CLAIM);
            task->base_url = g_strdup(relay_url);
            task->code = g_strdup(pairing_code);
            set_status(app, "Подключаем Kindle…");
            start_async_task(task);
        }
    }
    gtk_widget_destroy(dialog);
}

static gboolean poll_delivery(gpointer userdata) {
    DeliveryPoll *poll = userdata;
    AsyncTask *task;
    if (poll->request_pending) return TRUE;
    poll->request_pending = TRUE;
    task = async_task_new(poll->app, TASK_DELIVERY_STATUS);
    copy_common_task_fields(task, poll->app);
    task->job_id = g_strdup(poll->job_id);
    task->delivery_poll = poll;
    start_async_task(task);
    return TRUE;
}

static void settings_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Настройки", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "Отмена", GTK_RESPONSE_CANCEL, "Сохранить", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *table = gtk_table_new(1, 2, FALSE);
    GtkWidget *relay = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(relay), app->config->relay_url);
    gtk_table_attach_defaults(GTK_TABLE(table), gtk_label_new("Relay URL"), 0, 1, 0, 1);
    gtk_table_attach_defaults(GTK_TABLE(table), relay, 1, 2, 0, 1);
    gtk_container_set_border_width(GTK_CONTAINER(table), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), table, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_free(app->config->relay_url); app->config->relay_url = g_strdup(gtk_entry_get_text(GTK_ENTRY(relay)));
        bookrelay_config_save(app->config, app->config_path, NULL);
        set_status(app, "Настройки сохранены");
    }
    gtk_widget_destroy(dialog);
}

static void exit_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *dialog;
    if (app->active_tasks > 0) {
        set_status(app, "Дождитесь завершения операции перед выходом");
        return;
    }
    dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Выйти из BookRelay Kindle?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Все настройки и pairing останутся сохранены.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Отмена", GTK_RESPONSE_CANCEL, "Выйти", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) gtk_main_quit();
    gtk_widget_destroy(dialog);
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer userdata) {
    App *app = userdata;
    if (app->active_tasks > 0) {
        set_status(app, "Дождитесь завершения операции перед выходом");
        return TRUE;
    }
    return FALSE;
}

static gboolean async_task_complete(gpointer userdata) {
    AsyncTask *task = userdata;
    App *app = task->app;
    switch (task->kind) {
        case TASK_SEARCH:
            if (!task->books) {
                show_error(app, "Поиск не выполнен", task->error);
            } else if (task->books->len == 0 && task->page > 1) {
                gtk_widget_set_sensitive(app->next_page, FALSE);
                set_status(app, "Это последняя страница");
            } else {
                app->page = task->page;
                render_books(app, task->books);
                gtk_widget_set_sensitive(app->previous_page, task->page > 1);
                gtk_widget_set_sensitive(app->next_page, task->books->len > 0);
                set_status(app, "Поиск завершён");
            }
            break;
        case TASK_CATEGORIES:
            if (task->categories) {
                guint i;
                for (i = 0; i < task->categories->len; i++) gtk_combo_box_append_text(GTK_COMBO_BOX(app->categories), g_ptr_array_index(task->categories, i));
            }
            break;
        case TASK_PAIR_CLAIM:
            if (!task->claim || !task->claim->token || !*task->claim->token) {
                show_error(app, "Pairing не выполнен", task->error);
            } else {
                g_free(app->config->relay_url); app->config->relay_url = g_strdup(task->base_url);
                g_free(app->config->token); app->config->token = g_strdup(task->claim->token);
                g_free(app->config->kindle_email); app->config->kindle_email = g_strdup(task->claim->kindle_email ? task->claim->kindle_email : "");
                bookrelay_config_save(app->config, app->config_path, NULL);
                set_status(app, "Kindle привязан");
            }
            break;
        case TASK_SEND:
            if (!task->job_id) {
                show_error(app, "Не удалось создать задание", task->error);
            } else {
                DeliveryPoll *poll = g_new0(DeliveryPoll, 1);
                poll->app = app;
                poll->job_id = g_strdup(task->job_id);
                poll->source_id = g_timeout_add_seconds(3, poll_delivery, poll);
                set_status(app, "Задание создано, relay готовит EPUB…");
            }
            break;
        case TASK_DELIVERY_STATUS:
            if (task->delivery_poll) {
                DeliveryPoll *poll = task->delivery_poll;
                poll->request_pending = FALSE;
                poll->attempts++;
                if (!task->state && poll->attempts < 20) break;
                if (!task->state) {
                    show_error(app, "Не удалось получить статус доставки", task->error);
                } else if (g_strcmp0(task->state, "sent") == 0) {
                    set_status(app, "Relay отправил EPUB; ожидайте доставку Amazon");
                } else if (g_strcmp0(task->state, "failed") == 0) {
                    set_status(app, "Relay не смог отправить EPUB");
                } else if (poll->attempts < 20) {
                    break;
                } else {
                    set_status(app, "Задание всё ещё выполняется; проверьте статус relay позже");
                }
                if (poll->source_id) g_source_remove(poll->source_id);
                g_free(poll->job_id);
                g_free(poll);
            }
            break;
        case TASK_COVER:
            if (task->cover_bytes) {
                gchar *path = cover_cache_path(task->book_id);
                gchar *directory = g_path_get_dirname(path);
                gsize length = task->cover_bytes->len;
                const guint8 *data = task->cover_bytes->data;
                if (cache_has_room(directory, path, length)) g_file_set_contents(path, (const gchar *)data, (gssize)length, NULL);
                g_free(directory);
                {
                    GdkPixbuf *pixbuf = pixbuf_from_bytes(task->cover_bytes);
                    if (pixbuf) {
                        gtk_image_set_from_pixbuf(GTK_IMAGE(task->image), pixbuf);
                        g_object_unref(pixbuf);
                    }
                }
                g_free(path);
            }
            break;
    }
    if (app->active_tasks > 0) app->active_tasks--;
    async_task_free(task);
    return FALSE;
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
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->categories), "Все категории");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->categories), 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->query, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->categories, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), pair_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), settings_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), exit_button, FALSE, FALSE, 0);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), app->results);
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
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), app);
    gtk_widget_set_sensitive(previous_page, FALSE);
    gtk_widget_set_sensitive(next_page, FALSE);
    gtk_widget_show_all(app->window);
    if (app->config->token && *app->config->token) {
        AsyncTask *task = async_task_new(app, TASK_CATEGORIES);
        copy_common_task_fields(task, app);
        start_async_task(task);
    }
}

int main(int argc, char **argv) {
    App app = {0};
    const gchar *home = g_get_user_data_dir();
    app.config_path = g_build_filename(home, "bookrelay", "config.ini", NULL);
    app.config = bookrelay_config_load(app.config_path);
#if !GLIB_CHECK_VERSION(2, 32, 0)
    g_thread_init(NULL);
#endif
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    build_ui(&app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_main();
    bookrelay_config_free(app.config);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
