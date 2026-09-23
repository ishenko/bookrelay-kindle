#include "api.h"
#include "config.h"

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <string.h>

#define APP_NAME "BookRelay Kindle"
#define KINDLE_APP_WINDOW_TITLE "L:A_N:application_PC:T_ID:bookrelay.kindle"
#define KINDLE_DIALOG_WINDOW_TITLE "L:D_N:dialog_M:dismissable_ID:bookrelay.kindle.dialog"
#define MAX_COVER_CACHE_BYTES (64u * 1024u * 1024u)

typedef struct {
    BookRelayConfig *config;
    gchar *config_path;
    GtkWidget *window;
    GtkWidget *pages;
    GtkWidget *page_window;
    GtkWidget *query;
    GtkWidget *categories;
    GtkWidget *results;
    GtkWidget *status;
    GtkWidget *connection;
    GtkWidget *previous_page;
    GtkWidget *next_page;
    GtkWidget *keyboard;
    guint page;
    guint active_tasks;
    guint category_count;
    GPtrArray *category_ids;
} App;

typedef struct {
    GtkWidget *root;
    GtkEntry *target;
    gboolean cyrillic;
    gboolean shift;
    GPtrArray *letter_buttons;
} VirtualKeyboard;

typedef struct {
    App *app;
    GtkWidget *window;
    GtkEntry *relay;
    GtkEntry *code;
    VirtualKeyboard *keyboard;
} PairPage;

typedef struct {
    App *app;
    GtkWidget *window;
    GtkEntry *relay;
    GtkEntry *email;
    GtkWidget *auto_download;
    VirtualKeyboard *keyboard;
} SettingsPage;

typedef struct {
    App *app;
    GtkWidget *window;
    BookRelayBook *book;
} DetailsPage;

typedef struct { App *app; BookRelayBook *book; } BookRow;

typedef struct {
    App *app;
    gchar *job_id;
    guint attempts;
    guint source_id;
    gboolean request_pending;
} DeliveryPoll;

typedef enum {
    TASK_SEARCH, TASK_CATEGORIES, TASK_PAIR_CLAIM,
    TASK_SEND, TASK_DELIVERY_STATUS, TASK_COVER
} TaskKind;

typedef struct {
    App *app;
    TaskKind kind;
    gchar *base_url;
    gchar *token;
    gchar *query;
    gchar *category;
    gchar *code;
    gchar *book_id;
    gchar *title;
    gchar *job_id;
    gchar *url;
    guint page;
    GtkWidget *image;
    GtkWidget *placeholder;
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
static void make_touch_target(GtkWidget *widget, gint width, gint height);
static void set_large_font(GtkWidget *widget, const gchar *description);

static void virtual_keyboard_free(VirtualKeyboard *keyboard) {
    if (!keyboard) return;
    if (keyboard->letter_buttons) g_ptr_array_free(keyboard->letter_buttons, TRUE);
    g_free(keyboard);
}

static void virtual_keyboard_set_labels(VirtualKeyboard *keyboard) {
    guint i;
    if (!keyboard || !keyboard->letter_buttons) return;
    for (i = 0; i < keyboard->letter_buttons->len; i++) {
        GtkWidget *button = g_ptr_array_index(keyboard->letter_buttons, i);
        const gchar *latin = g_object_get_data(G_OBJECT(button), "bookrelay-key-latin");
        const gchar *cyrillic = g_object_get_data(G_OBJECT(button), "bookrelay-key-cyrillic");
        const gchar *label = keyboard->cyrillic ? cyrillic : latin;
        gchar *display = keyboard->shift ? g_utf8_strup(label, -1) : g_strdup(label);
        gtk_button_set_label(GTK_BUTTON(button), display);
        set_large_font(button, "Sans 24");
        g_free(display);
    }
}

static void virtual_keyboard_show_for(VirtualKeyboard *keyboard, GtkEntry *entry) {
    if (!keyboard || !entry) return;
    keyboard->target = entry;
    /* no-show-all keeps this hidden when a page is shown. Its children were
     * shown at construction; show() explicitly reveals the keyboard itself. */
    gtk_widget_show(keyboard->root);
    virtual_keyboard_set_labels(keyboard);
}

static gboolean virtual_keyboard_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer userdata) {
    VirtualKeyboard *keyboard = userdata;
    virtual_keyboard_show_for(keyboard, GTK_ENTRY(widget));
    return FALSE;
}

static gboolean virtual_keyboard_button_press(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    VirtualKeyboard *keyboard = userdata;
    if (event && event->type == GDK_BUTTON_PRESS) {
        virtual_keyboard_show_for(keyboard, GTK_ENTRY(widget));
        gtk_widget_grab_focus(widget);
    }
    return FALSE;
}

static void virtual_keyboard_bind(VirtualKeyboard *keyboard, GtkEntry *entry) {
    if (!keyboard || !entry) return;
    gtk_widget_add_events(GTK_WIDGET(entry), GDK_BUTTON_PRESS_MASK);
    g_signal_connect(entry, "focus-in-event", G_CALLBACK(virtual_keyboard_focus_in), keyboard);
    g_signal_connect(entry, "button-press-event", G_CALLBACK(virtual_keyboard_button_press), keyboard);
}

static void virtual_keyboard_insert(VirtualKeyboard *keyboard, const gchar *value) {
    gint position;
    gchar *text;
    if (!keyboard || !keyboard->target || !value) return;
    position = gtk_editable_get_position(GTK_EDITABLE(keyboard->target));
    text = g_strdup(value);
    if (keyboard->shift && g_utf8_validate(text, -1, NULL)) {
        gchar *upper = g_utf8_strup(text, -1);
        g_free(text);
        text = upper;
    }
    gtk_editable_insert_text(GTK_EDITABLE(keyboard->target), text, -1, &position);
    gtk_editable_set_position(GTK_EDITABLE(keyboard->target), position);
    g_free(text);
}

static void virtual_keyboard_backspace(VirtualKeyboard *keyboard) {
    gint position;
    gint selection_start;
    gint selection_end;
    if (!keyboard || !keyboard->target) return;
    if (gtk_editable_get_selection_bounds(GTK_EDITABLE(keyboard->target), &selection_start, &selection_end)) {
        gtk_editable_delete_text(GTK_EDITABLE(keyboard->target), selection_start, selection_end);
        gtk_editable_set_position(GTK_EDITABLE(keyboard->target), selection_start);
        return;
    }
    position = gtk_editable_get_position(GTK_EDITABLE(keyboard->target));
    if (position > 0) {
        gtk_editable_delete_text(GTK_EDITABLE(keyboard->target), position - 1, position);
        gtk_editable_set_position(GTK_EDITABLE(keyboard->target), position - 1);
    }
}

static void virtual_keyboard_clicked(GtkButton *button, gpointer userdata) {
    VirtualKeyboard *keyboard = userdata;
    const gchar *key = g_object_get_data(G_OBJECT(button), "bookrelay-key");
    if (g_strcmp0(key, "backspace") == 0) {
        virtual_keyboard_backspace(keyboard);
    } else if (g_strcmp0(key, "space") == 0) {
        virtual_keyboard_insert(keyboard, " ");
    } else if (g_strcmp0(key, "shift") == 0) {
        keyboard->shift = !keyboard->shift;
        virtual_keyboard_set_labels(keyboard);
    } else if (g_strcmp0(key, "language") == 0) {
        keyboard->cyrillic = !keyboard->cyrillic;
        keyboard->shift = FALSE;
        gtk_button_set_label(button, keyboard->cyrillic ? "EN" : "РУС");
        set_large_font(GTK_WIDGET(button), "Sans 18");
        virtual_keyboard_set_labels(keyboard);
    } else if (g_strcmp0(key, "done") == 0) {
        gtk_widget_hide(keyboard->root);
    } else if (g_strcmp0(key, "enter") == 0) {
        if (keyboard->target) g_signal_emit_by_name(keyboard->target, "activate");
        gtk_widget_hide(keyboard->root);
    } else {
        virtual_keyboard_insert(keyboard, key);
        if (keyboard->shift) {
            keyboard->shift = FALSE;
            virtual_keyboard_set_labels(keyboard);
        }
    }
}

static GtkWidget *virtual_keyboard_button(VirtualKeyboard *keyboard, const gchar *label, const gchar *key) {
    GtkWidget *button = gtk_button_new_with_label(label);
    make_touch_target(button, 48, MAX(42, gdk_screen_get_height(gdk_screen_get_default()) / 22));
    set_large_font(button, "Sans 24");
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NORMAL);
    g_object_set_data_full(G_OBJECT(button), "bookrelay-key", g_strdup(key), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(virtual_keyboard_clicked), keyboard);
    return button;
}

static GtkWidget *virtual_keyboard_letter(VirtualKeyboard *keyboard, const gchar *latin, const gchar *cyrillic) {
    GtkWidget *button = virtual_keyboard_button(keyboard, latin, latin);
    g_object_set_data(G_OBJECT(button), "bookrelay-key-latin", (gpointer)latin);
    g_object_set_data(G_OBJECT(button), "bookrelay-key-cyrillic", (gpointer)cyrillic);
    g_ptr_array_add(keyboard->letter_buttons, button);
    return button;
}

static void virtual_keyboard_pack_row(GtkWidget *root, GtkWidget *row) {
    gtk_box_pack_start(GTK_BOX(root), row, FALSE, FALSE, 1);
}

static VirtualKeyboard *virtual_keyboard_new(GtkWidget *parent, GtkEntry *initial_target) {
    static const gchar *latin_row_1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
    static const gchar *cyrillic_row_1[] = {"й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з"};
    static const gchar *latin_row_2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
    static const gchar *cyrillic_row_2[] = {"ф", "ы", "в", "а", "п", "р", "о", "л", "д"};
    static const gchar *latin_row_3[] = {"z", "x", "c", "v", "b", "n", "m"};
    static const gchar *cyrillic_row_3[] = {"я", "ч", "с", "м", "и", "т", "ь"};
    VirtualKeyboard *keyboard = g_new0(VirtualKeyboard, 1);
    GtkWidget *row;
    GtkWidget *button;
    guint i;
    keyboard->root = gtk_vbox_new(FALSE, 1);
    keyboard->target = initial_target;
    keyboard->letter_buttons = g_ptr_array_new();
    g_object_set_data_full(G_OBJECT(keyboard->root), "bookrelay-keyboard-state", keyboard, (GDestroyNotify)virtual_keyboard_free);
    gtk_container_set_border_width(GTK_CONTAINER(keyboard->root), 2);

    row = gtk_hbox_new(TRUE, 1);
    for (i = 0; i < 10; i++) {
        button = virtual_keyboard_letter(keyboard, latin_row_1[i], cyrillic_row_1[i]);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    virtual_keyboard_pack_row(keyboard->root, row);

    row = gtk_hbox_new(TRUE, 1);
    for (i = 0; i < 9; i++) {
        button = virtual_keyboard_letter(keyboard, latin_row_2[i], cyrillic_row_2[i]);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    virtual_keyboard_pack_row(keyboard->root, row);

    row = gtk_hbox_new(TRUE, 1);
    for (i = 0; i < 7; i++) {
        button = virtual_keyboard_letter(keyboard, latin_row_3[i], cyrillic_row_3[i]);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    button = virtual_keyboard_button(keyboard, ".", ".");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "@", "@");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "/", "/");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    virtual_keyboard_pack_row(keyboard->root, row);

    row = gtk_hbox_new(TRUE, 1);
    for (i = 0; i < 10; i++) {
        gchar label[2];
        label[0] = (gchar)('0' + i);
        label[1] = 0;
        button = virtual_keyboard_button(keyboard, label, label);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    virtual_keyboard_pack_row(keyboard->root, row);

    row = gtk_hbox_new(TRUE, 1);
    button = virtual_keyboard_button(keyboard, "РУС", "language");
    set_large_font(button, "Sans 18");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Shift", "shift");
    set_large_font(button, "Sans 18");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Пробел", "space");
    set_large_font(button, "Sans 18");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Удалить", "backspace");
    set_large_font(button, "Sans 18");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Готово", "done");
    set_large_font(button, "Sans 18");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    virtual_keyboard_pack_row(keyboard->root, row);

    gtk_widget_show_all(keyboard->root);
    gtk_widget_hide(keyboard->root);
    gtk_widget_set_no_show_all(keyboard->root, TRUE);
    if (parent) gtk_box_pack_start(GTK_BOX(parent), keyboard->root, FALSE, FALSE, 2);
    virtual_keyboard_bind(keyboard, initial_target);
    return keyboard;
}

static void set_kindle_dialog_role(GtkWidget *dialog) {
    gtk_window_set_title(GTK_WINDOW(dialog), KINDLE_DIALOG_WINDOW_TITLE);
}

static void set_large_font(GtkWidget *widget, const gchar *description) {
    PangoFontDescription *font = pango_font_description_from_string(description);
    gtk_widget_modify_font(widget, font);
    /* GtkButton owns a GtkLabel; GTK2 does not inherit the button font. */
    if (GTK_IS_BUTTON(widget) && gtk_bin_get_child(GTK_BIN(widget)))
        gtk_widget_modify_font(gtk_bin_get_child(GTK_BIN(widget)), font);
    pango_font_description_free(font);
}

static void make_touch_target(GtkWidget *widget, gint width, gint height) {
    gtk_widget_set_size_request(widget, width, height);
    set_large_font(widget, "Sans 17");
}

static void ink_color(GdkColor *color, const gchar *hex) {
    gdk_color_parse(hex, color);
}

static void ink_background(GtkWidget *widget, const gchar *hex) {
    GdkColor color;
    ink_color(&color, hex);
    gtk_widget_modify_bg(widget, GTK_STATE_NORMAL, &color);
}

static void ink_text(GtkWidget *widget, const gchar *hex) {
    GdkColor color;
    ink_color(&color, hex);
    gtk_widget_modify_fg(widget, GTK_STATE_NORMAL, &color);
}

static void ink_primary_button(GtkWidget *button) {
    GdkColor ink;
    GdkColor paper;
    ink_color(&ink, "#111111");
    ink_color(&paper, "#ffffff");
    gtk_widget_modify_bg(button, GTK_STATE_NORMAL, &ink);
    gtk_widget_modify_bg(button, GTK_STATE_PRELIGHT, &ink);
    gtk_widget_modify_bg(button, GTK_STATE_ACTIVE, &ink);
    if (gtk_bin_get_child(GTK_BIN(button))) {
        GtkWidget *label = gtk_bin_get_child(GTK_BIN(button));
        gtk_widget_modify_fg(label, GTK_STATE_NORMAL, &paper);
        gtk_widget_modify_fg(label, GTK_STATE_PRELIGHT, &paper);
        gtk_widget_modify_fg(label, GTK_STATE_ACTIVE, &paper);
    }
}

static GtkWidget *ink_header(const gchar *eyebrow, const gchar *heading) {
    GtkWidget *background = gtk_event_box_new();
    GtkWidget *content = gtk_vbox_new(FALSE, 5);
    GtkWidget *overline = gtk_label_new(eyebrow);
    GtkWidget *title = gtk_label_new(heading);
    ink_background(background, "#111111");
    ink_text(overline, "#ffffff");
    ink_text(title, "#ffffff");
    set_large_font(overline, "Sans Bold 12");
    set_large_font(title, "Sans Bold 28");
    gtk_misc_set_alignment(GTK_MISC(overline), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    gtk_box_pack_start(GTK_BOX(content), overline, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(background), content);
    return background;
}

static GtkWidget *ink_section(const gchar *text) {
    GtkWidget *label = gtk_label_new(text);
    set_large_font(label, "Sans Bold 14");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    return label;
}

static void page_window_destroyed(GtkWidget *window, gpointer userdata) {
    App *app = userdata;
    if (app && app->page_window == window) app->page_window = NULL;
}

static GtkWidget *new_kindle_page(App *app, const gchar *heading, GtkWidget **body_out, GtkWidget **actions_out) {
    GtkWidget *root;
    GtkWidget *header;
    GtkWidget *scroll;
    GtkWidget *body;
    GtkWidget *actions;

    if (app->page_window) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app->pages), gtk_notebook_page_num(GTK_NOTEBOOK(app->pages), app->page_window));
        return NULL;
    }

    root = gtk_vbox_new(FALSE, 8);
    header = ink_header("BOOKRELAY  /  KINDLE", heading);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    body = gtk_vbox_new(FALSE, 10);
    actions = gtk_hbox_new(TRUE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(body), 8);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), body);
    ink_background(gtk_bin_get_child(GTK_BIN(scroll)), "#ffffff");
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(app->pages), root, NULL);
    g_signal_connect(root, "destroy", G_CALLBACK(page_window_destroyed), app);
    app->page_window = root;
    /* GTK2 will not select a hidden notebook page. The controls are added and
     * shown by the caller immediately after this page becomes active. */
    gtk_widget_show(root);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->pages), gtk_notebook_page_num(GTK_NOTEBOOK(app->pages), root));
    if (body_out) *body_out = body;
    if (actions_out) *actions_out = actions;
    return root;
}

static void attach_page_keyboard(GtkWidget *page_root, VirtualKeyboard *keyboard) {
    if (!page_root || !keyboard || !keyboard->root) return;
    gtk_box_pack_start(GTK_BOX(page_root), keyboard->root, FALSE, FALSE, 4);
    gtk_box_reorder_child(GTK_BOX(page_root), keyboard->root, 2);
}

static GtkWidget *page_button(const gchar *label) {
    GtkWidget *button = gtk_button_new_with_label(label);
    make_touch_target(button, 180, 54);
    return button;
}

static gboolean focus_widget_idle(gpointer userdata) {
    GtkWidget *widget = GTK_WIDGET(userdata);
    if (GTK_IS_WIDGET(widget)) gtk_widget_grab_focus(widget);
    return FALSE;
}

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
    g_free(task->category);
    g_free(task->code);
    g_free(task->book_id);
    g_free(task->title);
    g_free(task->job_id);
    g_free(task->url);
    if (task->image) g_object_unref(task->image);
    if (task->placeholder) g_object_unref(task->placeholder);
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
            task->books = bookrelay_api_search(task->base_url, task->token, task->query, task->category, (gint)task->page, &task->error);
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
    gtk_label_set_text(GTK_LABEL(app->status), message ? message : "");
}

static void show_error(App *app, const gchar *prefix, GError *error) {
    gchar *message = g_strdup_printf("%s: %s", prefix, error ? error->message : "неизвестная ошибка");
    set_status(app, message);
    g_free(message);
}

static void update_connection(App *app) {
    if (app->config->token && *app->config->token)
        gtk_label_set_text(GTK_LABEL(app->connection), "●  Relay подключён · книги можно отправлять");
    else
        gtk_label_set_text(GTK_LABEL(app->connection), "○  Relay не подключён · откройте «Подключение»");
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
    if (!bytes || !bytes->len) return NULL;
    loader = gdk_pixbuf_loader_new();
    if (!gdk_pixbuf_loader_write(loader, bytes->data, bytes->len, NULL) || !gdk_pixbuf_loader_close(loader, NULL)) {
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
    gint cover_width = CLAMP(gdk_screen_get_width(gdk_screen_get_default()) / 8, 104, 160);
    gint cover_height = cover_width * 146 / 104;
    GdkPixbuf *pixbuf = NULL;
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    GtkWidget *image = gtk_image_new();
    GtkWidget *placeholder = gtk_label_new("Нет обложки");
    gtk_widget_set_size_request(image, cover_width, cover_height);
    gtk_widget_set_size_request(placeholder, cover_width, cover_height);
    /* render_books() calls gtk_widget_show_all() after cards are built. Keep
     * the mutually exclusive placeholder from being re-shown by that call. */
    gtk_widget_set_no_show_all(placeholder, TRUE);
    gtk_misc_set_alignment(GTK_MISC(placeholder), 0.5, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(placeholder), TRUE);
    set_large_font(placeholder, "Sans 14");
    gtk_box_pack_start(GTK_BOX(box), image, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), placeholder, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    gtk_widget_set_size_request(frame, cover_width + 12, cover_height + 16);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) pixbuf = gdk_pixbuf_new_from_file_at_scale(path, cover_width, cover_height, TRUE, NULL);
    if (pixbuf) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        gtk_widget_hide(placeholder);
        g_object_unref(pixbuf);
    } else if (book->cover_url && *book->cover_url) {
        AsyncTask *task = async_task_new(app, TASK_COVER);
        copy_common_task_fields(task, app);
        task->book_id = g_strdup(book->id);
        task->url = g_strdup(book->cover_url);
        task->image = g_object_ref(image);
        task->placeholder = g_object_ref(placeholder);
        gtk_widget_show(placeholder);
        start_async_task(task);
    } else {
        gtk_widget_show(placeholder);
    }
    g_free(path);
    return frame;
}

static void clear_results(App *app) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->results));
    GList *item;
    for (item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
}

static void render_empty_state(App *app, const gchar *message) {
    GtkWidget *box = gtk_vbox_new(FALSE, 14);
    GtkWidget *eyebrow = ink_section("ВАША БИБЛИОТЕКА НА KINDLE");
    GtkWidget *title = gtk_label_new(message);
    GtkWidget *hint = gtk_label_new(app->config->token && *app->config->token
        ? "Найдите книгу по названию или автору. Выберите категорию, если хотите сузить поиск."
        : "Подключите relay, чтобы искать книги и отправлять их на Kindle. Нажмите «Подключение» внизу экрана.");
    GtkWidget *rule = gtk_hseparator_new();
    set_large_font(title, "Sans Bold 26");
    set_large_font(hint, "Sans 17");
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(hint), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), eyebrow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), rule, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(box), 28);
    gtk_box_pack_start(GTK_BOX(app->results), box, FALSE, FALSE, 8);
    gtk_widget_show_all(app->results);
    gtk_widget_queue_resize(app->results);
    gtk_widget_queue_draw(app->results);
}

static gchar *list_excerpt(const gchar *text, guint max_chars) {
    gchar *prefix;
    gchar *excerpt;
    gchar *last_space;
    if (!text || !*text) return g_strdup("");
    if (g_utf8_strlen(text, -1) <= max_chars) return g_strdup(text);
    prefix = g_utf8_substring(text, 0, max_chars);
    last_space = g_strrstr(prefix, " ");
    if (last_space && g_utf8_strlen(prefix, last_space - prefix) > max_chars / 2)
        *last_space = 0;
    excerpt = g_strconcat(prefix, "…", NULL);
    g_free(prefix);
    return excerpt;
}

static void render_books(App *app, GPtrArray *books) {
    guint i;
    clear_results(app);
    if (!books || books->len == 0) {
        render_empty_state(app, "Ничего не найдено");
        return;
    }
    for (i = 0; i < books->len; i++) {
        BookRelayBook *book = g_ptr_array_index(books, i);
        GtkWidget *frame = gtk_frame_new(NULL);
        GtkWidget *row = gtk_hbox_new(FALSE, 14);
        GtkWidget *text_box = gtk_vbox_new(FALSE, 4);
        gchar *title_text = list_excerpt(book->title && *book->title ? book->title : "Без названия", 76);
        gchar *author_text = list_excerpt(book->author && *book->author ? book->author : "Автор не указан", 54);
        GtkWidget *title = gtk_label_new(title_text);
        GtkWidget *author = gtk_label_new(author_text);
        GtkWidget *meta = gtk_label_new("Книга");
        GtkWidget *button = gtk_button_new_with_label("Подробнее");
        BookRow *data = g_new0(BookRow, 1);
        g_free(title_text);
        g_free(author_text);
        data->app = app;
        data->book = bookrelay_book_copy(book);
        g_object_set_data_full(G_OBJECT(button), "book-row", data, (GDestroyNotify)book_row_free);
        g_signal_connect(button, "clicked", G_CALLBACK(show_details), data);
        gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
        gtk_misc_set_alignment(GTK_MISC(author), 0, 0.5);
        gtk_misc_set_alignment(GTK_MISC(meta), 0, 0.5);
        gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(author), TRUE);
        set_large_font(title, "Sans Bold 17");
        set_large_font(author, "Sans 14");
        set_large_font(meta, "Sans Bold 12");
        gtk_label_set_text(GTK_LABEL(meta), "КНИГА  /  EPUB");
        make_touch_target(button, 170, 48);
        ink_primary_button(button);
        gtk_box_pack_start(GTK_BOX(row), make_cover(app, book), FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(text_box), title, FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(text_box), author, FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(text_box), meta, FALSE, FALSE, 4);
        gtk_box_pack_end(GTK_BOX(text_box), button, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(row), text_box, TRUE, TRUE, 5);
        gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
        gtk_container_set_border_width(GTK_CONTAINER(frame), 12);
        gtk_container_add(GTK_CONTAINER(frame), row);
        gtk_box_pack_start(GTK_BOX(app->results), frame, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(app->results), gtk_hseparator_new(), FALSE, FALSE, 2);
    }
    gtk_widget_show_all(app->results);
    gtk_widget_queue_resize(app->results);
    gtk_widget_queue_draw(app->results);
}

static void details_page_close(GtkButton *button, gpointer userdata) {
    DetailsPage *page = userdata;
    gtk_widget_destroy(page->window);
}

static void details_page_send(GtkButton *button, gpointer userdata) {
    DetailsPage *page = userdata;
    App *app = page->app;
    if (!app->config->token || !*app->config->token) {
        set_status(app, "Сначала выполните pairing в настройках");
        gtk_widget_destroy(page->window);
        return;
    }
    {
        AsyncTask *task = async_task_new(app, TASK_SEND);
        set_status(app, "Отправляем книгу на Kindle…");
        copy_common_task_fields(task, app);
        task->book_id = g_strdup(page->book->id);
        task->title = g_strdup(page->book->title);
        start_async_task(task);
    }
    gtk_widget_destroy(page->window);
}

static void details_page_free(DetailsPage *page) {
    if (!page) return;
    bookrelay_book_free(page->book);
    g_free(page);
}

static void show_details(GtkButton *button, gpointer userdata) {
    BookRow *row = userdata;
    GtkWidget *body;
    GtkWidget *actions;
    GtkWidget *window;
    GtkWidget *cover;
    GtkWidget *label;
    GtkWidget *close_button;
    GtkWidget *send_button;
    DetailsPage *page;
    gchar *text;

    window = new_kindle_page(row->app, "Карточка книги", &body, &actions);
    if (!window) return;
    page = g_new0(DetailsPage, 1);
    page->app = row->app;
    page->window = window;
    page->book = bookrelay_book_copy(row->book);
    g_object_set_data_full(G_OBJECT(window), "bookrelay-details-page", page, (GDestroyNotify)details_page_free);

    cover = make_cover(row->app, page->book);
    label = gtk_label_new(NULL);
    text = g_strdup_printf("%s\n\nАвтор: %s\n\n%s", page->book->title, page->book->author, page->book->description && *page->book->description ? page->book->description : "Описание отсутствует.");
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
    set_large_font(label, "Sans 18");
    gtk_widget_set_size_request(label, MAX(200, gdk_screen_get_width(gdk_screen_get_default()) - 96), -1);
    {
        GtkWidget *cover_alignment = gtk_alignment_new(0, 0, 0, 0);
        gtk_container_add(GTK_CONTAINER(cover_alignment), cover);
        gtk_box_pack_start(GTK_BOX(body), cover_alignment, FALSE, FALSE, 4);
    }
    gtk_box_pack_start(GTK_BOX(body), label, FALSE, FALSE, 12);

    close_button = page_button("Назад");
    send_button = page_button("Скачать на Kindle");
    g_signal_connect(close_button, "clicked", G_CALLBACK(details_page_close), page);
    g_signal_connect(send_button, "clicked", G_CALLBACK(details_page_send), page);
    ink_primary_button(send_button);
    gtk_box_pack_start(GTK_BOX(actions), close_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), send_button, TRUE, TRUE, 0);
    gtk_widget_show_all(window);
    g_free(text);
}

static const gchar *selected_category(App *app) {
    gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->categories));
    if (active <= 0 || !app->category_ids || (guint)(active - 1) >= app->category_ids->len) return "";
    return g_ptr_array_index(app->category_ids, active - 1);
}

static void search_page(App *app, guint page) {
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(app->query));
    const gchar *category = selected_category(app);
    AsyncTask *task;
    if ((!query || !*query) && (!category || !*category)) {
        clear_results(app);
        render_empty_state(app, "Введите запрос");
        set_status(app, "Укажите название, автора или категорию");
        gtk_window_set_focus(GTK_WINDOW(app->window), app->query);
        return;
    }
    set_status(app, "Ищем книги…");
    gtk_widget_set_sensitive(app->previous_page, FALSE);
    gtk_widget_set_sensitive(app->next_page, FALSE);
    task = async_task_new(app, TASK_SEARCH);
    copy_common_task_fields(task, app);
    task->query = g_strdup(query ? query : "");
    task->category = g_strdup(category ? category : "");
    task->page = page;
    start_async_task(task);
}

static void search_clicked(GtkButton *button, gpointer userdata) { search_page((App *)userdata, 1); }
static void search_entry_activate(GtkEntry *entry, gpointer userdata) { search_page((App *)userdata, 1); }

static void previous_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (app->page > 1) search_page(app, app->page - 1);
}

static void next_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    search_page(app, app->page + 1);
}

static gchar *trim_relay_url(const gchar *value) {
    gchar *url = g_strdup(value ? value : "");
    gchar *end;
    g_strstrip(url);
    end = url + strlen(url);
    while (end > url && end[-1] == '/') *--end = '\0';
    return url;
}

static gboolean valid_relay_url(const gchar *value) {
    return value && *value && (g_str_has_prefix(value, "https://") || g_str_has_prefix(value, "http://")) && !strchr(value, ' ');
}

static void load_categories(App *app) {
    AsyncTask *task;
    if (!app->config->token || !*app->config->token) return;
    task = async_task_new(app, TASK_CATEGORIES);
    copy_common_task_fields(task, app);
    start_async_task(task);
}

static void pair_page_cancel(GtkButton *button, gpointer userdata) {
    PairPage *page = userdata;
    gtk_widget_destroy(page->window);
}

static void pair_page_connect(GtkButton *button, gpointer userdata) {
    PairPage *page = userdata;
    App *app = page->app;
    const gchar *pairing_code = gtk_entry_get_text(page->code);
    gchar *relay_url = trim_relay_url(gtk_entry_get_text(page->relay));

    if (!valid_relay_url(relay_url)) {
        set_status(app, "Relay URL должен начинаться с http:// или https://");
        gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->relay));
        virtual_keyboard_show_for(page->keyboard, page->relay);
        g_free(relay_url);
        return;
    }
    if (!pairing_code || !*pairing_code) {
        set_status(app, "Введите одноразовый код");
        gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->code));
        virtual_keyboard_show_for(page->keyboard, page->code);
        g_free(relay_url);
        return;
    }
    {
        AsyncTask *task = async_task_new(app, TASK_PAIR_CLAIM);
        task->base_url = g_strdup(relay_url);
        task->code = g_strdup(pairing_code);
        set_status(app, "Подключаем Kindle…");
        start_async_task(task);
    }
    g_free(relay_url);
    gtk_widget_destroy(page->window);
}

static void pair_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *body;
    GtkWidget *actions;
    GtkWidget *window;
    GtkWidget *intro;
    GtkWidget *relay_label;
    GtkWidget *code_label;
    GtkWidget *connect_button;
    GtkWidget *cancel_button;
    PairPage *page;

    window = new_kindle_page(app, "Подключение", &body, &actions);
    if (!window) return;
    page = g_new0(PairPage, 1);
    page->app = app;
    page->window = window;
    page->relay = GTK_ENTRY(gtk_entry_new());
    page->code = GTK_ENTRY(gtk_entry_new());
    g_object_set_data_full(G_OBJECT(window), "bookrelay-pair-page", page, g_free);

    intro = gtk_label_new("Введите URL сервера и одноразовый код.");
    relay_label = gtk_label_new("Адрес relay");
    code_label = gtk_label_new("Код подключения");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_misc_set_alignment(GTK_MISC(intro), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(relay_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(code_label), 0, 0.5);
    set_large_font(intro, "Sans 17");
    set_large_font(relay_label, "Sans Bold 18");
    set_large_font(code_label, "Sans Bold 18");
    set_large_font(GTK_WIDGET(page->relay), "Sans 20");
    set_large_font(GTK_WIDGET(page->code), "Sans 20");
    gtk_entry_set_width_chars(page->relay, 8);
    gtk_entry_set_width_chars(page->code, 8);
    gtk_widget_set_size_request(GTK_WIDGET(page->relay), -1, 56);
    gtk_widget_set_size_request(GTK_WIDGET(page->code), -1, 56);
    gtk_entry_set_text(page->relay, app->config->relay_url ? app->config->relay_url : "");
    gtk_entry_set_max_length(page->code, 32);
    gtk_box_pack_start(GTK_BOX(body), intro, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), relay_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->relay), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), code_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->code), FALSE, FALSE, 4);
    page->keyboard = virtual_keyboard_new(NULL, page->relay);
    virtual_keyboard_bind(page->keyboard, page->code);
    attach_page_keyboard(window, page->keyboard);

    cancel_button = page_button("Отмена");
    connect_button = page_button("Подключить");
    ink_primary_button(connect_button);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(pair_page_cancel), page);
    g_signal_connect(connect_button, "clicked", G_CALLBACK(pair_page_connect), page);
    gtk_box_pack_start(GTK_BOX(actions), cancel_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), connect_button, TRUE, TRUE, 0);
    gtk_widget_show_all(window);
    virtual_keyboard_show_for(page->keyboard, page->relay);
    gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->relay));
    g_idle_add(focus_widget_idle, page->relay);
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

static void settings_page_cancel(GtkButton *button, gpointer userdata) {
    SettingsPage *page = userdata;
    gtk_widget_destroy(page->window);
}

static void settings_page_save(GtkButton *button, gpointer userdata) {
    SettingsPage *page = userdata;
    App *app = page->app;
    gchar *relay_url = trim_relay_url(gtk_entry_get_text(page->relay));
    if (!valid_relay_url(relay_url)) {
        set_status(app, "Relay URL должен начинаться с http:// или https://");
        gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->relay));
        virtual_keyboard_show_for(page->keyboard, page->relay);
        g_free(relay_url);
        return;
    }
    g_free(app->config->relay_url);
    app->config->relay_url = relay_url;
    app->config->auto_download = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->auto_download));
    bookrelay_config_save(app->config, app->config_path, NULL);
    set_status(app, "Настройки сохранены");
    update_connection(app);
    gtk_widget_destroy(page->window);
}

static void settings_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *body;
    GtkWidget *actions;
    GtkWidget *window;
    GtkWidget *relay_label;
    GtkWidget *email_label;
    GtkWidget *hint;
    GtkWidget *cancel_button;
    GtkWidget *save_button;
    SettingsPage *page;

    window = new_kindle_page(app, "Настройки", &body, &actions);
    if (!window) return;
    page = g_new0(SettingsPage, 1);
    page->app = app;
    page->window = window;
    page->relay = GTK_ENTRY(gtk_entry_new());
    page->email = GTK_ENTRY(gtk_entry_new());
    page->auto_download = gtk_check_button_new_with_label("Автоматически отправлять новые задания");
    g_object_set_data_full(G_OBJECT(window), "bookrelay-settings-page", page, g_free);

    relay_label = gtk_label_new("Relay URL");
    email_label = gtk_label_new("Почта Kindle");
    hint = gtk_label_new("Адрес почты задаётся при подключении и здесь доступен только для просмотра.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(hint), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(relay_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(email_label), 0, 0.5);
    set_large_font(relay_label, "Sans Bold 18");
    set_large_font(email_label, "Sans Bold 18");
    set_large_font(hint, "Sans 16");
    set_large_font(GTK_WIDGET(page->relay), "Sans 20");
    set_large_font(GTK_WIDGET(page->email), "Sans 20");
    set_large_font(page->auto_download, "Sans 17");
    gtk_entry_set_width_chars(page->relay, 8);
    gtk_entry_set_width_chars(page->email, 8);
    gtk_entry_set_text(page->relay, app->config->relay_url ? app->config->relay_url : "");
    gtk_entry_set_text(page->email, app->config->kindle_email ? app->config->kindle_email : "");
    gtk_widget_set_sensitive(GTK_WIDGET(page->email), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->auto_download), app->config->auto_download);
    gtk_box_pack_start(GTK_BOX(body), relay_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->relay), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), email_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->email), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), hint, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), page->auto_download, FALSE, FALSE, 12);
    page->keyboard = virtual_keyboard_new(NULL, page->relay);
    attach_page_keyboard(window, page->keyboard);

    cancel_button = page_button("Отмена");
    save_button = page_button("Сохранить");
    ink_primary_button(save_button);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(settings_page_cancel), page);
    g_signal_connect(save_button, "clicked", G_CALLBACK(settings_page_save), page);
    gtk_box_pack_start(GTK_BOX(actions), cancel_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), save_button, TRUE, TRUE, 0);
    gtk_widget_show_all(window);
    virtual_keyboard_show_for(page->keyboard, page->relay);
    gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->relay));
    g_idle_add(focus_widget_idle, page->relay);
}

static void exit_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *dialog;
    if (app->active_tasks > 0) {
        set_status(app, "Дождитесь завершения операции перед выходом");
        return;
    }
    dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Выйти из BookRelay Kindle?");
    set_kindle_dialog_role(dialog);
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
                gchar *message;
                app->page = task->page;
                render_books(app, task->books);
                gtk_widget_set_sensitive(app->previous_page, task->page > 1);
                gtk_widget_set_sensitive(app->next_page, task->books->len > 0);
                message = g_strdup_printf("Найдено книг: %u", task->books->len);
                set_status(app, message);
                g_free(message);
            }
            break;
        case TASK_CATEGORIES:
            if (!task->categories) {
                show_error(app, "Категории не загрузились", task->error);
            } else {
                guint i;
                while (app->category_count > 0) {
                    gtk_combo_box_remove_text(GTK_COMBO_BOX(app->categories), app->category_count);
                    app->category_count--;
                }
                g_ptr_array_set_size(app->category_ids, 0);
                for (i = 0; i < task->categories->len; i++) {
                    BookRelayCategory *category = g_ptr_array_index(task->categories, i);
                    gtk_combo_box_append_text(GTK_COMBO_BOX(app->categories), category->title);
                    g_ptr_array_add(app->category_ids, g_strdup(category->id));
                    app->category_count++;
                }
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->categories), 0);
                set_status(app, "Категории загружены");
            }
            break;
        case TASK_PAIR_CLAIM:
            if (!task->claim || !task->claim->token || !*task->claim->token) {
                show_error(app, "Pairing не выполнен", task->error);
            } else {
                g_free(app->config->relay_url);
                app->config->relay_url = g_strdup(task->base_url);
                g_free(app->config->token);
                app->config->token = g_strdup(task->claim->token);
                g_free(app->config->kindle_email);
                app->config->kindle_email = g_strdup(task->claim->kindle_email ? task->claim->kindle_email : "");
                bookrelay_config_save(app->config, app->config_path, NULL);
                update_connection(app);
                set_status(app, "Kindle привязан");
                load_categories(app);
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
                if (!task->state) show_error(app, "Не удалось получить статус доставки", task->error);
                else if (g_strcmp0(task->state, "sent") == 0) set_status(app, "Relay отправил EPUB; ожидайте доставку Amazon");
                else if (g_strcmp0(task->state, "failed") == 0) set_status(app, "Relay не смог отправить EPUB");
                else if (poll->attempts < 20) break;
                else set_status(app, "Задание всё ещё выполняется; проверьте статус relay позже");
                if (poll->source_id) g_source_remove(poll->source_id);
                g_free(poll->job_id);
                g_free(poll);
            }
            break;
        case TASK_COVER:
            if (task->cover_bytes) {
                gchar *path = cover_cache_path(task->book_id);
                gchar *directory = g_path_get_dirname(path);
                GdkPixbuf *pixbuf = pixbuf_from_bytes(task->cover_bytes);
                if (cache_has_room(directory, path, task->cover_bytes->len)) g_file_set_contents(path, (const gchar *)task->cover_bytes->data, (gssize)task->cover_bytes->len, NULL);
                if (pixbuf) {
                    gtk_image_set_from_pixbuf(GTK_IMAGE(task->image), pixbuf);
                    if (task->placeholder) gtk_widget_hide(task->placeholder);
                    g_object_unref(pixbuf);
                }
                g_free(directory);
                g_free(path);
            }
            break;
    }
    if (app->active_tasks > 0) app->active_tasks--;
    async_task_free(task);
    return FALSE;
}

static void build_ui(App *app) {
    GtkWidget *root = gtk_vbox_new(FALSE, 10);
    GtkWidget *header = ink_header("БИБЛИОТЕКА  /  KINDLE", "BookRelay");
    GtkWidget *shell = gtk_vbox_new(FALSE, 0);
    GtkWidget *search_row = gtk_hbox_new(FALSE, 8);
    GtkWidget *category_row = gtk_vbox_new(FALSE, 4);
    GtkWidget *actions = gtk_hbox_new(TRUE, 6);
    GtkWidget *navigation = gtk_hbox_new(TRUE, 6);
    GtkWidget *search_section = ink_section("01   ПОИСК КНИГИ");
    GtkWidget *results_section = ink_section("02   РЕЗУЛЬТАТЫ");
    GtkWidget *search_button = gtk_button_new_with_label("Искать");
    GtkWidget *category_label = ink_section("КАТЕГОРИЯ");
    GtkWidget *pair_button = gtk_button_new_with_label("Подключение");
    GtkWidget *settings_button = gtk_button_new_with_label("Настройки");
    GtkWidget *exit_button = gtk_button_new_with_label("Выйти");
    GtkWidget *previous_page = gtk_button_new_with_label("Назад");
    GtkWidget *next_page = gtk_button_new_with_label("Дальше");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    app->pages = gtk_notebook_new();
    app->query = gtk_entry_new();
    app->categories = gtk_combo_box_new_text();
    app->results = gtk_vbox_new(FALSE, 6);
    app->status = gtk_label_new("Подключите relay для поиска");
    app->connection = gtk_label_new("Relay не подключён");
    app->category_ids = g_ptr_array_new_with_free_func(g_free);
    app->previous_page = previous_page;
    app->next_page = next_page;
    app->page = 1;

    gtk_window_set_title(GTK_WINDOW(app->window), KINDLE_APP_WINDOW_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(app->window),
                                gdk_screen_get_width(gdk_screen_get_default()),
                                gdk_screen_get_height(gdk_screen_get_default()));
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);
    ink_background(app->window, "#ffffff");
    set_large_font(app->connection, "Sans 14");
    set_large_font(app->status, "Sans 14");
    set_large_font(app->query, "Sans 19");
    set_large_font(app->categories, "Sans 16");
    gtk_misc_set_alignment(GTK_MISC(app->connection), 0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(app->connection), TRUE);
    gtk_misc_set_alignment(GTK_MISC(app->status), 0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(app->status), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(app->query), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(app->query), 12);
    gtk_widget_set_tooltip_text(app->query, "Название или автор");
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->categories), "Все категории");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->categories), 0);
    make_touch_target(search_button, 128, 50);
    ink_primary_button(search_button);
    make_touch_target(pair_button, 140, 48);
    make_touch_target(settings_button, 150, 48);
    make_touch_target(exit_button, 112, 48);
    make_touch_target(previous_page, 150, 48);
    make_touch_target(next_page, 150, 48);
    gtk_widget_set_size_request(app->query, -1, 50);
    gtk_widget_set_size_request(app->categories, -1, 48);
    gtk_widget_set_size_request(app->status, -1, 30);
    /* The results pane expands when the keyboard is hidden, and yields space
     * to the five keyboard rows on the shorter Paperwhite viewport. */
    gtk_widget_set_size_request(scroll, -1, 100);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), app->results);
    ink_background(gtk_bin_get_child(GTK_BIN(scroll)), "#ffffff");
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(app->pages), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(app->pages), FALSE);

    gtk_box_pack_start(GTK_BOX(search_row), app->query, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(category_row), category_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(category_row), app->categories, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), previous_page, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), next_page, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), pair_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), settings_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), exit_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->connection, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(root), gtk_hseparator_new(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), search_section, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(root), search_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), category_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), results_section, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(root), gtk_hseparator_new(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    {
        VirtualKeyboard *keyboard = virtual_keyboard_new(root, GTK_ENTRY(app->query));
        app->keyboard = keyboard->root;
    }
    gtk_box_pack_start(GTK_BOX(root), navigation, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(app->pages), root, NULL);
    gtk_box_pack_start(GTK_BOX(shell), app->pages, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(shell), app->status, FALSE, FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(shell), 8);
    gtk_container_add(GTK_CONTAINER(app->window), shell);
    g_signal_connect(search_button, "clicked", G_CALLBACK(search_clicked), app);
    g_signal_connect(app->query, "activate", G_CALLBACK(search_entry_activate), app);
    g_signal_connect(pair_button, "clicked", G_CALLBACK(pair_clicked), app);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(settings_clicked), app);
    g_signal_connect(exit_button, "clicked", G_CALLBACK(exit_clicked), app);
    g_signal_connect(previous_page, "clicked", G_CALLBACK(previous_page_clicked), app);
    g_signal_connect(next_page, "clicked", G_CALLBACK(next_page_clicked), app);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), app);
    gtk_widget_set_sensitive(previous_page, FALSE);
    gtk_widget_set_sensitive(next_page, FALSE);
    update_connection(app);
    render_empty_state(app, app->config->token && *app->config->token ? "Готово к поиску" : "Подключите relay");
    gtk_window_set_focus(GTK_WINDOW(app->window), search_button);
    gtk_widget_show_all(app->window);
    gtk_widget_hide(app->keyboard);
    if (app->config->token && *app->config->token) load_categories(app);
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
    g_ptr_array_free(app.category_ids, TRUE);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
