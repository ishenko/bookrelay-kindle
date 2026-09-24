#include "api.h"
#include "config.h"
#include "favorites.h"

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
#define MAX_PENDING_COVERS 36u

typedef struct {
    BookRelayConfig *config;
    gchar *config_path;
    BookRelayFavorites *favorites;
    GtkWidget *window;
    GtkWidget *pages;
    GtkWidget *page_window;
    GtkWidget *query;
    GtkWidget *search_row;
    GtkWidget *search_icon;
    guint search_press_timeout;
    gboolean search_press_handled;
    GtkWidget *header_title;
    GtkWidget *home_button;
    GtkWidget *breadcrumb_row;
    GtkWidget *breadcrumb_category;
    GtkWidget *breadcrumb_current;
    GtkWidget *breadcrumb_separator;
    GtkWidget *section_title;
    GtkWidget *results;
    GtkWidget *status;
    GtkWidget *previous_page;
    GtkWidget *next_page;
    GtkWidget *first_page;
    GtkWidget *last_page;
    GtkWidget *page_label;
    GtkWidget *keyboard;
    GtkWidget *delivery_page;
    GtkWidget *delivery_label;
    guint columns;
    guint page;
    guint active_tasks;
    guint generation;
    gint cover_generation;
    guint pending_covers;
    GQueue deferred_covers;
    guint view;
    gboolean has_next;
    gboolean catalog_ready;
    GPtrArray *catalog_categories;
    GPtrArray *subcategories;
    gchar *category_id;
    gchar *category_title;
    gchar *subcategory_id;
    gchar *subcategory_title;
} App;

enum { VIEW_CATEGORIES, VIEW_SUBCATEGORIES, VIEW_BOOKS, VIEW_SEARCH, VIEW_FAVORITES };
typedef struct { App *app; gchar *id; gchar *title; } CategoryRow;

typedef struct {
    GtkWidget *root;
    GtkWidget *bottom_widget;
    GtkWidget *symbols_button;
    GtkEntry *target;
    gboolean cyrillic;
    gboolean shift;
    gboolean symbols;
    gboolean native_open;
    gboolean native_unavailable;
    gboolean defer_search_open;
    GPtrArray *letter_buttons;
} VirtualKeyboard;

typedef struct {
    App *app;
    GtkWidget *window;
    GtkEntry *relay;
    GtkEntry *email;
    GtkEntry *code;
    GtkWidget *feedback;
    gboolean claimed;
    VirtualKeyboard *keyboard;
} SettingsPage;

typedef struct {
    App *app;
    GtkWidget *window;
    BookRelayBook *book;
    GtkWidget *favorite_button;
    GtkWidget *favorite_image;
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
    TASK_SEND, TASK_DELIVERY_STATUS, TASK_COVER, TASK_SUBCATEGORIES, TASK_CATALOG_BOOKS,
    TASK_UPDATE_EMAIL
} TaskKind;

typedef struct {
    App *app;
    TaskKind kind;
    gchar *base_url;
    gchar *token;
    gchar *query;
    gchar *category;
    gchar *subcategory;
    gchar *code;
    gchar *email;
    gchar *book_id;
    gchar *title;
    gchar *job_id;
    gchar *url;
    guint page;
    guint size;
    guint generation;
    gboolean has_next;
    gint cover_width;
    gint cover_height;
    GtkWidget *image;
    GtkWidget *placeholder;
    GtkWidget *settings_page;
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
static void navigate_view(App *app, guint view, guint page);
static void make_touch_target(GtkWidget *widget, gint width, gint height);
static void set_large_font(GtkWidget *widget, const gchar *description);
static void settings_clicked(GtkButton *button, gpointer userdata);
static void exit_clicked(GtkButton *button, gpointer userdata);
static GtkWidget *ink_icon_image(const gchar *filename);
static void set_icon_image(GtkWidget *image, const gchar *filename);
static void render_books(App *app, GPtrArray *books);

static void reload_favorites(App *app) {
    bookrelay_favorites_free(app->favorites);
    app->favorites = bookrelay_favorites_load(app->config_path, app->config->relay_url);
}

static gboolean kindle_keyboard_property(const gchar *property, const gchar *value) {
    const gchar *path = g_getenv("BOOKRELAY_LIPC_SET_PROP");
    gchar *argv[6];
    gint status = -1;
    if (!path || !*path) path = "/usr/bin/lipc-set-prop";
    if (!g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) return FALSE;
    argv[0] = (gchar *)path;
    argv[1] = "-s";
    argv[2] = "com.lab126.keyboard";
    argv[3] = (gchar *)property;
    argv[4] = (gchar *)value;
    argv[5] = NULL;
    return g_spawn_sync(NULL, argv, NULL,
                        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, NULL, NULL, &status, NULL) && status == 0;
}

static void virtual_keyboard_free(VirtualKeyboard *keyboard) {
    if (!keyboard) return;
    if (keyboard->native_open)
        kindle_keyboard_property("close", "bookrelay.kindle");
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
        const gchar *symbol = g_object_get_data(G_OBJECT(button), "bookrelay-key-symbol");
        const gchar *label = keyboard->symbols ? symbol : (keyboard->cyrillic ? cyrillic : latin);
        gchar *display = keyboard->shift && !keyboard->symbols ? g_utf8_strup(label, -1) : g_strdup(label);
        gtk_button_set_label(GTK_BUTTON(button), display);
        set_large_font(button, "Sans 24");
        g_free(display);
    }
}

static void virtual_keyboard_show_for(VirtualKeyboard *keyboard, GtkEntry *entry) {
    GList *children, *item;
    gboolean native;
    if (!keyboard || !entry) return;
    keyboard->target = entry;
    /* Prefer the device keyboard when LIPC is available. The built-in one is
     * still available if LIPC fails. */
    native = g_strcmp0(g_getenv("BOOKRELAY_KEYBOARD"), "custom") != 0 &&
             !keyboard->native_unavailable;
    if (native && !keyboard->native_open) {
        keyboard->native_open = kindle_keyboard_property("open", "bookrelay.kindle:abc:0");
        if (!keyboard->native_open) keyboard->native_unavailable = TRUE;
    }
    native = native && keyboard->native_open;
    children = gtk_container_get_children(GTK_CONTAINER(keyboard->root));
    for (item = children; item; item = item->next) {
        GtkWidget *row = item->data;
        gtk_widget_set_no_show_all(row, native);
        if (native) gtk_widget_hide(row);
        else gtk_widget_show_all(row);
    }
    g_list_free(children);
    /* The Kindle keyboard overlays the page; reserving height here creates
     * a large gray blank area and pushes the form out of view. */
    if (native) gtk_widget_hide(keyboard->root);
    else gtk_widget_show(keyboard->root);
    if (!native) virtual_keyboard_set_labels(keyboard);
}

static void virtual_keyboard_hide(VirtualKeyboard *keyboard) {
    if (!keyboard) return;
    if (keyboard->native_open) {
        kindle_keyboard_property("close", "bookrelay.kindle");
        keyboard->native_open = FALSE;
    }
    gtk_widget_hide(keyboard->root);
}

static void virtual_keyboard_entry_activate(GtkEntry *entry, gpointer userdata) {
    virtual_keyboard_hide(userdata);
}

static gboolean virtual_keyboard_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer userdata) {
    VirtualKeyboard *keyboard = userdata;
    if (!keyboard->defer_search_open)
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

static gboolean virtual_keyboard_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer userdata) {
    VirtualKeyboard *keyboard = userdata;
    /* The Kindle keyboard can temporarily take X focus while it is accepting
     * input. Closing it here interrupts the search entry before the first
     * character arrives. Enter, a background tap, and page changes still
     * close it explicitly. */
    if (!keyboard->native_open) virtual_keyboard_hide(keyboard);
    return FALSE;
}

static void virtual_keyboard_bind(VirtualKeyboard *keyboard, GtkEntry *entry) {
    if (!keyboard || !entry) return;
    gtk_widget_add_events(GTK_WIDGET(entry), GDK_BUTTON_PRESS_MASK);
    g_signal_connect(entry, "focus-in-event", G_CALLBACK(virtual_keyboard_focus_in), keyboard);
    g_signal_connect(entry, "focus-out-event", G_CALLBACK(virtual_keyboard_focus_out), keyboard);
    g_signal_connect(entry, "button-press-event", G_CALLBACK(virtual_keyboard_button_press), keyboard);
    g_signal_connect(entry, "activate", G_CALLBACK(virtual_keyboard_entry_activate), keyboard);
}

static gboolean virtual_keyboard_background_press(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    App *app = userdata;
    GtkWidget *target = gtk_get_event_widget((GdkEvent *)event);
    GtkWidget *cursor;
    VirtualKeyboard *keyboard = NULL;
    if (app->page_window) {
        SettingsPage *settings = g_object_get_data(G_OBJECT(app->page_window), "bookrelay-settings-page");
        if (settings) keyboard = settings->keyboard;
    } else if (app->keyboard) {
        keyboard = g_object_get_data(G_OBJECT(app->keyboard), "bookrelay-keyboard-state");
    }
    if (!keyboard) return FALSE;
    for (cursor = target; cursor; cursor = gtk_widget_get_parent(cursor))
        if (GTK_IS_ENTRY(cursor) || cursor == keyboard->root) return FALSE;
    virtual_keyboard_hide(keyboard);
    return FALSE;
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
    } else if (g_strcmp0(key, "symbols") == 0) {
        keyboard->symbols = !keyboard->symbols;
        keyboard->shift = FALSE;
        gtk_button_set_label(button, keyboard->symbols ? "АБВ" : "?123");
        virtual_keyboard_set_labels(keyboard);
    } else if (g_strcmp0(key, "language") == 0) {
        keyboard->cyrillic = !keyboard->cyrillic;
        keyboard->symbols = FALSE;
        keyboard->shift = FALSE;
        gtk_button_set_label(button, keyboard->cyrillic ? "EN" : "РУС");
        gtk_button_set_label(GTK_BUTTON(keyboard->symbols_button), "?123");
        set_large_font(GTK_WIDGET(button), "Sans 15");
        virtual_keyboard_set_labels(keyboard);
    } else if (g_strcmp0(key, "done") == 0) {
        virtual_keyboard_hide(keyboard);
    } else if (g_strcmp0(key, "enter") == 0) {
        if (keyboard->target) g_signal_emit_by_name(keyboard->target, "activate");
        virtual_keyboard_hide(keyboard);
    } else {
        const gchar *letter = g_object_get_data(G_OBJECT(button), "bookrelay-key-latin");
        if (letter)
            key = keyboard->symbols
                ? g_object_get_data(G_OBJECT(button), "bookrelay-key-symbol")
                : (keyboard->cyrillic ? g_object_get_data(G_OBJECT(button), "bookrelay-key-cyrillic") : letter);
        if (g_strcmp0(key, "https://") == 0) keyboard->shift = FALSE;
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

static GtkWidget *virtual_keyboard_letter(VirtualKeyboard *keyboard, const gchar *latin, const gchar *cyrillic, const gchar *symbol) {
    GtkWidget *button = virtual_keyboard_button(keyboard, latin, latin);
    g_object_set_data(G_OBJECT(button), "bookrelay-key-latin", (gpointer)latin);
    g_object_set_data(G_OBJECT(button), "bookrelay-key-cyrillic", (gpointer)cyrillic);
    g_object_set_data(G_OBJECT(button), "bookrelay-key-symbol", (gpointer)symbol);
    g_ptr_array_add(keyboard->letter_buttons, button);
    return button;
}

static void virtual_keyboard_pack_row(GtkWidget *root, GtkWidget *row) {
    gtk_box_pack_start(GTK_BOX(root), row, FALSE, FALSE, 1);
}

static VirtualKeyboard *virtual_keyboard_new(GtkWidget *parent, GtkEntry *initial_target) {
    static const gchar *latin_row_1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
    static const gchar *cyrillic_row_1[] = {"й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з"};
    static const gchar *symbol_row_1[] = {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"};
    static const gchar *latin_row_2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
    static const gchar *cyrillic_row_2[] = {"ф", "ы", "в", "а", "п", "р", "о", "л", "д"};
    static const gchar *symbol_row_2[] = {"-", "_", "=", "+", "[", "]", "{", "}", ":"};
    static const gchar *latin_row_3[] = {"z", "x", "c", "v", "b", "n", "m"};
    static const gchar *cyrillic_row_3[] = {"я", "ч", "с", "м", "и", "т", "ь"};
    static const gchar *symbol_row_3[] = {";", "'", "\"", "?", "\\", "<", ">"};
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
        button = virtual_keyboard_letter(keyboard, latin_row_1[i], cyrillic_row_1[i], symbol_row_1[i]);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    virtual_keyboard_pack_row(keyboard->root, row);

    row = gtk_hbox_new(TRUE, 1);
    for (i = 0; i < 9; i++) {
        button = virtual_keyboard_letter(keyboard, latin_row_2[i], cyrillic_row_2[i], symbol_row_2[i]);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    virtual_keyboard_pack_row(keyboard->root, row);

    row = gtk_hbox_new(TRUE, 1);
    for (i = 0; i < 7; i++) {
        button = virtual_keyboard_letter(keyboard, latin_row_3[i], cyrillic_row_3[i], symbol_row_3[i]);
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
    button = virtual_keyboard_button(keyboard, "https://", "https://");
    set_large_font(button, "Sans 15");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "РУС", "language");
    set_large_font(button, "Sans 15");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Shift", "shift");
    set_large_font(button, "Sans 15");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "?123", "symbols");
    keyboard->symbols_button = button;
    set_large_font(button, "Sans 15");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Пробел", "space");
    set_large_font(button, "Sans 15");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Удалить", "backspace");
    set_large_font(button, "Sans 15");
    gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    button = virtual_keyboard_button(keyboard, "Готово", "done");
    set_large_font(button, "Sans 15");
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
    /* Pango strings use points; Kindle reports a high DPI and would triple
     * their pixel size. Keep the size seen in our 96-DPI layout previews. */
    pango_font_description_set_absolute_size(font,
        pango_font_description_get_size(font) * (96.0 / 72.0));
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
    set_large_font(overline, "Sans 12");
    set_large_font(title, "Sans 26");
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

static gboolean show_setup_if_needed(gpointer userdata) {
    App *app = userdata;
    if (!app->catalog_ready && !app->page_window)
        settings_clicked(NULL, app);
    return FALSE;
}

static void keep_setup_open(GtkWidget *window, gpointer userdata) {
    App *app = userdata;
    if (!app->catalog_ready)
        g_idle_add(show_setup_if_needed, app);
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
    gtk_widget_add_events(gtk_bin_get_child(GTK_BIN(scroll)), GDK_BUTTON_PRESS_MASK);
    g_signal_connect(gtk_bin_get_child(GTK_BIN(scroll)), "button-press-event",
                     G_CALLBACK(virtual_keyboard_background_press), app);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(root), "bookrelay-page-actions", actions);
    if (app->keyboard)
        virtual_keyboard_hide(g_object_get_data(G_OBJECT(app->keyboard), "bookrelay-keyboard-state"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->pages), root, NULL);
    g_signal_connect(root, "destroy", G_CALLBACK(page_window_destroyed), app);
    g_signal_connect(root, "destroy", G_CALLBACK(keep_setup_open), app);
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
    keyboard->bottom_widget = g_object_get_data(G_OBJECT(page_root), "bookrelay-page-actions");
    gtk_box_pack_start(GTK_BOX(page_root), keyboard->root, FALSE, FALSE, 4);
    gtk_box_reorder_child(GTK_BOX(page_root), keyboard->root, 2);
}

static GtkWidget *page_button(const gchar *label) {
    GtkWidget *button = gtk_button_new_with_label(label);
    make_touch_target(button, 225, 68);
    return button;
}

static gboolean focus_widget_idle(gpointer userdata) {
    GtkWidget *widget = GTK_WIDGET(userdata);
    if (GTK_WIDGET_MAPPED(widget)) gtk_widget_grab_focus(widget);
    return FALSE;
}

static void refocus_search_entry(App *app) {
    gint position = gtk_editable_get_position(GTK_EDITABLE(app->query));
    /* GTK may select all text when an entry receives focus again. Preserve
     * the caret so the next Kindle character appends instead of replacing. */
    gtk_window_set_focus(GTK_WINDOW(app->window), NULL);
    gtk_widget_grab_focus(app->query);
    gtk_editable_select_region(GTK_EDITABLE(app->query), position, position);
}

static gboolean focus_search_idle(gpointer userdata) {
    App *app = userdata;
    VirtualKeyboard *keyboard = g_object_get_data(G_OBJECT(app->keyboard), "bookrelay-keyboard-state");
    if (!app->page_window && app->view == VIEW_SEARCH && GTK_WIDGET_MAPPED(app->query)) {
        /* Settings fields open the Kindle keyboard before taking entry focus.
         * Use the same order for search, after the icon release has finished. */
        virtual_keyboard_show_for(keyboard, GTK_ENTRY(app->query));
        keyboard->defer_search_open = FALSE;
        /* The entry may still be GTK's logical focus from before the Kindle
         * overlay took X focus. Re-enter it to activate its input method. */
        refocus_search_entry(app);
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, focus_widget_idle,
                        g_object_ref(app->query), g_object_unref);
    } else {
        keyboard->defer_search_open = FALSE;
    }
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
    g_free(task->subcategory);
    g_free(task->code);
    g_free(task->email);
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
    task->generation = app->generation;
    app->active_tasks++;
    if (kind == TASK_COVER) app->pending_covers++;
    return task;
}

static void advance_generation(App *app) {
    while (!g_queue_is_empty(&app->deferred_covers))
        async_task_free(g_queue_pop_head(&app->deferred_covers));
    app->generation++;
    g_atomic_int_set(&app->cover_generation, (gint)app->generation);
}

static gpointer async_task_worker(gpointer userdata) {
    AsyncTask *task = userdata;
    switch (task->kind) {
        case TASK_SEARCH:
            task->books = bookrelay_api_search(task->base_url, task->token, task->query, NULL, (gint)task->page, (gint)task->size, &task->has_next, &task->error);
            break;
        case TASK_CATEGORIES:
            task->categories = bookrelay_api_categories(task->base_url, task->token, &task->error);
            break;
        case TASK_SUBCATEGORIES:
            task->categories = bookrelay_api_subcategories(task->base_url, task->token, task->category, &task->error);
            break;
        case TASK_CATALOG_BOOKS:
            task->books = bookrelay_api_catalog_books(task->base_url, task->token, task->category, task->subcategory, (gint)task->page, (gint)task->size, &task->has_next, &task->error);
            break;
        case TASK_PAIR_CLAIM:
            task->claim = bookrelay_api_pair_claim(task->base_url, task->code, &task->error);
            break;
        case TASK_UPDATE_EMAIL:
            task->state = bookrelay_api_update_email(task->base_url, task->token, task->email, &task->error);
            break;
        case TASK_SEND:
            task->job_id = bookrelay_api_send(task->base_url, task->token, task->book_id, task->title, &task->error);
            break;
        case TASK_DELIVERY_STATUS:
            task->state = bookrelay_api_delivery_status(task->base_url, task->token, task->job_id, &task->error);
            break;
        case TASK_COVER:
            /* Skip queued covers from pages the reader has already left.
             * GTK widgets are only released later on the main thread. */
            if (task->generation == (guint)g_atomic_int_get(&task->app->cover_generation))
                bookrelay_api_download(task->base_url, task->token, task->url, &task->cover_bytes, &task->error);
            break;
    }
    g_idle_add(async_task_complete, task);
    return NULL;
}

static void cover_task_worker(gpointer task, gpointer unused) {
    (void)unused;
    async_task_worker(task);
}

static gboolean start_async_task(AsyncTask *task) {
    GThread *thread;
    /* A book page may contain twelve covers. Keep those requests bounded so
     * slow source images cannot start twelve simultaneous network threads. */
    if (task->kind == TASK_COVER) {
        static GThreadPool *cover_pool = NULL;
        GError *pool_error = NULL;
        if (!cover_pool) cover_pool = g_thread_pool_new(cover_task_worker, NULL, 3, FALSE, &pool_error);
        if (cover_pool) {
            g_thread_pool_push(cover_pool, task, &pool_error);
            if (!pool_error) return TRUE;
        }
        if (pool_error) g_error_free(pool_error);
        task->app->active_tasks--;
        task->app->pending_covers--;
        async_task_free(task);
        return FALSE;
    }
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

static void start_deferred_covers(App *app) {
    while (app->pending_covers < MAX_PENDING_COVERS && !g_queue_is_empty(&app->deferred_covers)) {
        AsyncTask *task = g_queue_pop_head(&app->deferred_covers);
        app->active_tasks++;
        app->pending_covers++;
        start_async_task(task);
    }
}

static void copy_common_task_fields(AsyncTask *task, App *app) {
    task->base_url = g_strdup(app->config->relay_url);
    task->token = g_strdup(app->config->token);
}

static void set_status(App *app, const gchar *message) {
    gtk_label_set_text(GTK_LABEL(app->status), message ? message : "");
    if (message && *message) gtk_widget_show(app->status);
    else gtk_widget_hide(app->status);
}

static void show_error(App *app, const gchar *prefix, GError *error) {
    gchar *message = g_strdup_printf("%s: %s", prefix, error ? error->message : "неизвестная ошибка");
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

static gchar *list_excerpt(const gchar *text, guint max_chars);

static GtkWidget *book_placeholder(BookRelayBook *book, gint width, gint height) {
    GtkWidget *background = gtk_event_box_new();
    gboolean compact = width < 190;
    GtkWidget *content = gtk_vbox_new(FALSE, compact ? 5 : 8);
    GtkWidget *title;
    GtkWidget *author;
    GtkWidget *year;
    gchar *title_text = list_excerpt(book->title && *book->title ? book->title : "Без названия", compact ? 24 : 52);
    gchar *author_text = list_excerpt(book->author && *book->author ? book->author : "Автор не указан", compact ? 16 : 28);
    gchar *year_text = book->year ? g_strdup_printf("%d", book->year) : g_strdup("Год не указан");
    title = gtk_label_new(title_text);
    author = gtk_label_new(author_text);
    year = gtk_label_new(year_text);
    g_free(title_text); g_free(author_text); g_free(year_text);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(background), TRUE);
    ink_background(background, "#efefec");
    gtk_widget_set_size_request(background, width, height);
    gtk_container_set_border_width(GTK_CONTAINER(content), compact ? 6 : 15);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_line_wrap(GTK_LABEL(author), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(author), PANGO_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(title, width - (compact ? 12 : 30), -1);
    gtk_widget_set_size_request(author, width - (compact ? 12 : 30), -1);
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0);
    gtk_misc_set_alignment(GTK_MISC(author), 0, 1);
    gtk_misc_set_alignment(GTK_MISC(year), 0, 1);
    set_large_font(title, width < 150 ? "Sans Bold 10" : compact ? "Sans Bold 13" : "Sans Bold 16");
    set_large_font(author, compact ? "Sans 9" : "Sans 12");
    set_large_font(year, compact ? "Sans 9" : "Sans 12");
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(content), year, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(content), author, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(background), content);
    /* The placeholder root is later marked no_show_all, so reveal its
     * children before it is packed into the cover's toggled widget pair. */
    gtk_widget_show_all(background);
    return background;
}

static GtkWidget *make_cover(App *app, BookRelayBook *book, gint cover_width) {
    gchar *path = cover_cache_path(book->id);
    gint cover_height = cover_width * 3 / 2;
    GdkPixbuf *pixbuf = NULL;
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    GtkWidget *image = gtk_image_new();
    GtkWidget *placeholder = book_placeholder(book, cover_width, cover_height);
    gtk_widget_set_size_request(image, cover_width, cover_height);
    gtk_widget_set_size_request(placeholder, cover_width, cover_height);
    /* render_books() calls gtk_widget_show_all() after cards are built. Only
     * the chosen cover may become visible during that call. */
    gtk_widget_set_no_show_all(image, TRUE);
    gtk_widget_set_no_show_all(placeholder, TRUE);
    gtk_box_pack_start(GTK_BOX(box), image, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), placeholder, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    gtk_widget_set_size_request(frame, cover_width + 8, cover_height + 8);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) pixbuf = gdk_pixbuf_new_from_file_at_scale(path, cover_width, cover_height, TRUE, NULL);
    if (pixbuf) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        gtk_widget_show(image);
        gtk_widget_hide(placeholder);
        g_object_unref(pixbuf);
    } else if (book->cover_url && *book->cover_url) {
        AsyncTask *task = g_new0(AsyncTask, 1);
        task->app = app;
        task->kind = TASK_COVER;
        task->generation = app->generation;
        copy_common_task_fields(task, app);
        task->book_id = g_strdup(book->id);
        task->url = g_strdup(book->cover_url);
        task->cover_width = cover_width;
        task->cover_height = cover_height;
        task->image = g_object_ref(image);
        task->placeholder = g_object_ref(placeholder);
        gtk_widget_show(placeholder);
        if (app->pending_covers >= MAX_PENDING_COVERS) {
            /* Keep only the visible page waiting; leaving it releases these
             * widget references in advance_generation(). */
            g_queue_push_tail(&app->deferred_covers, task);
        } else {
            app->active_tasks++;
            app->pending_covers++;
            start_async_task(task);
        }
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
        ? "Найдите книгу по названию или автору или выберите категорию на главной."
        : "Подключите relay, чтобы искать книги и отправлять их на Kindle. Откройте настройки через шестерёнку вверху экрана.");
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

static const gchar *offline_genres[] = {
    "Старинное", "Компьютеры и Интернет", "Детская литература: прочее",
    "Детская литература: сказки", "Документальная литература", "Приключения",
    "Наука, Образование", "Религия, духовность, эзотерика", "Учебники и пособия",
    "Техника", "Фантастика", "Прочее", "Деловая литература", "Поэзия",
    "Проза", "Справочная литература", "Фольклор", "Детективы и триллеры",
    "Дом и семья", "Любовные романы", "Искусство, Искусствоведение, Дизайн",
    "Драматургия", "Детская художественная литература", "Юмор"
};

static void seed_categories(App *app) {
    guint i;
    app->catalog_categories = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_category_free);
    for (i = 0; i < G_N_ELEMENTS(offline_genres); i++) {
        BookRelayCategory *category = g_new0(BookRelayCategory, 1);
        category->title = g_strdup(offline_genres[i]);
        category->id = g_strdup(offline_genres[i]);
        g_ptr_array_add(app->catalog_categories, category);
    }
}

static guint items_per_page(App *app) {
    return 12;
}

static gint grid_cover_width(gboolean book) {
    gint width = gdk_screen_get_width(gdk_screen_get_default());
    gint height = gdk_screen_get_height(gdk_screen_get_default());
    gint max_width = book ? 200 : 230;
    gint height_budget = book ? (height - 650) / 5 : (height - 470) / 5;
    return MIN(MIN(max_width, (width - 120) / 4), MAX(book ? 110 : 104, height_budget));
}

static gint grid_label_width(gint cover_width) {
    gint screen_width = gdk_screen_get_width(gdk_screen_get_default());
    return cover_width < 150 ? MIN(156, (screen_width - 88) / 4) : cover_width;
}

static void update_pager(App *app) {
    guint count = 0, total;
    gchar *text;
    if (app->view == VIEW_CATEGORIES) count = app->catalog_categories ? app->catalog_categories->len : 0;
    if (app->view == VIEW_SUBCATEGORIES) count = app->subcategories ? app->subcategories->len : 0;
    if (app->view == VIEW_FAVORITES) count = app->favorites ? app->favorites->books->len : 0;
    total = MAX(1, (count + items_per_page(app) - 1) / items_per_page(app));
    if (app->view == VIEW_BOOKS || app->view == VIEW_SEARCH) {
        text = app->has_next ? g_strdup_printf("Страница %u", app->page)
                             : g_strdup_printf("Страница %u из %u", app->page, app->page);
    } else {
        text = g_strdup_printf("Страница %u из %u", app->page, total);
    }
    gtk_label_set_text(GTK_LABEL(app->page_label), text);
    gtk_widget_set_sensitive(app->first_page, app->page > 1);
    gtk_widget_set_sensitive(app->previous_page, app->page > 1);
    gtk_widget_set_sensitive(app->next_page, (app->view == VIEW_BOOKS || app->view == VIEW_SEARCH) ? app->has_next : app->page < total);
    gtk_widget_set_sensitive(app->last_page, (app->view == VIEW_CATEGORIES || app->view == VIEW_SUBCATEGORIES || app->view == VIEW_FAVORITES) && app->page < total);
    g_free(text);
}

static GtkWidget *asset_cover(const gchar *key, const gchar *title, gint width) {
    const gchar *asset_dir = g_getenv("BOOKRELAY_ASSET_DIR");
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    gchar *filename = g_strconcat(hash, ".jpg", NULL);
    gchar *path = g_build_filename(asset_dir && *asset_dir ? asset_dir : "client/share/covers", filename, NULL);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, width, width * 3 / 2, TRUE, NULL);
    GtkWidget *image;
    if (pixbuf) {
        image = gtk_image_new_from_pixbuf(pixbuf);
        g_object_unref(pixbuf);
    } else {
        GtkWidget *label = gtk_label_new(title);
        GtkWidget *background = gtk_event_box_new();
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_size_request(label, width - 20, -1);
        gtk_widget_set_size_request(background, width, width * 3 / 2);
        gtk_container_add(GTK_CONTAINER(background), label);
        ink_background(background, "#efefec");
        set_large_font(label, width < 150 ? "Sans Bold 12" : "Sans Bold 18");
        image = background;
    }
    g_free(path); g_free(filename); g_free(hash);
    return image;
}

static void category_clicked(GtkButton *button, gpointer userdata) {
    CategoryRow *row = userdata;
    App *app = row->app;
    if (!app->config->token || !*app->config->token) {
        set_status(app, "Откройте настройки и подключите relay, чтобы открыть категорию");
        return;
    }
    if (g_strcmp0(app->category_id, row->id) != 0 && app->subcategories) {
        g_ptr_array_free(app->subcategories, TRUE);
        app->subcategories = NULL;
    }
    g_free(app->category_id); g_free(app->category_title);
    app->category_id = g_strdup(row->id);
    app->category_title = g_strdup(row->title);
    navigate_view(app, VIEW_SUBCATEGORIES, 1);
}

static void subcategory_clicked(GtkButton *button, gpointer userdata) {
    CategoryRow *row = userdata;
    App *app = row->app;
    g_free(app->subcategory_id); g_free(app->subcategory_title);
    app->subcategory_id = g_strdup(row->id);
    app->subcategory_title = g_strdup(row->title);
    navigate_view(app, VIEW_BOOKS, 1);
}

static void category_row_free(CategoryRow *row) {
    g_free(row->id); g_free(row->title); g_free(row);
}

static void render_categories(App *app) {
    guint i, start = (app->page - 1) * items_per_page(app);
    gint cover_width = grid_cover_width(FALSE);
    gint label_width = grid_label_width(cover_width);
    GtkWidget *grid = gtk_table_new(3, 4, TRUE);
    gtk_table_set_row_spacings(GTK_TABLE(grid), cover_width < 150 ? 10 : 24);
    gtk_table_set_col_spacings(GTK_TABLE(grid), cover_width < 150 ? 8 : 16);
    clear_results(app);
    for (i = start; app->catalog_categories && i < app->catalog_categories->len && i < start + items_per_page(app); i++) {
        BookRelayCategory *category = g_ptr_array_index(app->catalog_categories, i);
        GtkWidget *button = gtk_button_new();
        GtkWidget *box = gtk_vbox_new(FALSE, 5);
        gchar *caption = list_excerpt(category->title, cover_width < 150 ? 31 : 45);
        GtkWidget *label = gtk_label_new(caption);
        GtkWidget *cover_align = gtk_alignment_new(0.5, 0, 0, 0);
        CategoryRow *row = g_new0(CategoryRow, 1);
        row->app = app; row->id = g_strdup(category->id); row->title = g_strdup(category->title);
        g_object_set_data_full(G_OBJECT(button), "category-row", row, (GDestroyNotify)category_row_free);
        g_signal_connect(button, "clicked", G_CALLBACK(category_clicked), row);
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_container_add(GTK_CONTAINER(cover_align), asset_cover(category->title, category->title, cover_width));
        gtk_box_pack_start(GTK_BOX(box), cover_align, FALSE, FALSE, 0);
        gtk_widget_set_tooltip_text(label, category->title);
        g_free(caption);
        set_large_font(label, cover_width < 150 ? "Sans 12" : "Sans 15");
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_width_chars(GTK_LABEL(label), MAX(9, label_width / 12));
        gtk_label_set_max_width_chars(GTK_LABEL(label), MAX(9, label_width / 12));
        gtk_widget_set_size_request(label, label_width, -1);
        gtk_widget_set_size_request(button, label_width + 14, -1);
        gtk_misc_set_alignment(GTK_MISC(label), 0.5, 0.5);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), box);
        gtk_table_attach_defaults(GTK_TABLE(grid), button, (i - start) % 4,
                                  (i - start) % 4 + 1, (i - start) / 4,
                                  (i - start) / 4 + 1);
    }
    gtk_box_pack_start(GTK_BOX(app->results), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(app->results);
    update_pager(app);
}

static void render_subcategories(App *app) {
    guint i, start = (app->page - 1) * items_per_page(app);
    gint cover_width = grid_cover_width(FALSE);
    GtkWidget *grid;
    clear_results(app);
    if (!app->subcategories) { render_empty_state(app, "Загружаем подкатегории…"); update_pager(app); return; }
    grid = gtk_table_new(3, 4, TRUE);
    gtk_table_set_row_spacings(GTK_TABLE(grid), cover_width < 150 ? 10 : 24);
    gtk_table_set_col_spacings(GTK_TABLE(grid), cover_width < 150 ? 8 : 16);
    for (i = start; i < app->subcategories->len && i < start + items_per_page(app); i++) {
        BookRelayCategory *category = g_ptr_array_index(app->subcategories, i);
        GtkWidget *button = gtk_button_new();
        GtkWidget *cover_align = gtk_alignment_new(0.5, 0, 0, 0);
        CategoryRow *row = g_new0(CategoryRow, 1);
        row->app = app; row->id = g_strdup(category->id); row->title = g_strdup(category->title);
        g_object_set_data_full(G_OBJECT(button), "subcategory-row", row, (GDestroyNotify)category_row_free);
        g_signal_connect(button, "clicked", G_CALLBACK(subcategory_clicked), row);
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_container_add(GTK_CONTAINER(cover_align), asset_cover(category->id, category->title, cover_width));
        gtk_widget_set_tooltip_text(button, category->title);
        gtk_widget_set_size_request(button, cover_width + 14, -1);
        gtk_container_add(GTK_CONTAINER(button), cover_align);
        gtk_table_attach_defaults(GTK_TABLE(grid), button, (i - start) % 4,
                                  (i - start) % 4 + 1, (i - start) / 4,
                                  (i - start) / 4 + 1);
    }
    gtk_box_pack_start(GTK_BOX(app->results), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(app->results);
    update_pager(app);
}

static void render_books(App *app, GPtrArray *books) {
    guint i, start = app->view == VIEW_FAVORITES ? (app->page - 1) * items_per_page(app) : 0;
    gint cover_width = grid_cover_width(TRUE);
    GtkWidget *grid = gtk_table_new(3, 4, TRUE);
    gtk_table_set_row_spacings(GTK_TABLE(grid), cover_width < 150 ? 8 : 24);
    gtk_table_set_col_spacings(GTK_TABLE(grid), cover_width < 150 ? 8 : 16);
    clear_results(app);
    if (!books || books->len == 0) {
        render_empty_state(app, app->view == VIEW_FAVORITES ? "В избранном пока нет книг" : "Ничего не найдено");
        update_pager(app);
        return;
    }
    for (i = start; i < books->len && i < start + items_per_page(app); i++) {
        BookRelayBook *book = g_ptr_array_index(books, i);
        GtkWidget *button = gtk_button_new();
        GtkWidget *cover_align = gtk_alignment_new(0.5, 0, 0, 0);
        BookRow *data = g_new0(BookRow, 1);
        data->app = app;
        data->book = bookrelay_book_copy(book);
        g_object_set_data_full(G_OBJECT(button), "book-row", data, (GDestroyNotify)book_row_free);
        g_signal_connect(button, "clicked", G_CALLBACK(show_details), data);
        gtk_widget_set_size_request(button, cover_width + 18, -1);
        gtk_container_add(GTK_CONTAINER(cover_align), make_cover(app, book, cover_width));
        gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
        gtk_container_add(GTK_CONTAINER(button), cover_align);
        gtk_table_attach_defaults(GTK_TABLE(grid), button, (i - start) % 4, (i - start) % 4 + 1,
                                  (i - start) / 4, (i - start) / 4 + 1);
    }
    gtk_box_pack_start(GTK_BOX(app->results), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(app->results);
    update_pager(app);
}

static void details_page_close(GtkButton *button, gpointer userdata) {
    DetailsPage *page = userdata;
    gtk_widget_destroy(page->window);
}

static void delivery_page_destroyed(GtkWidget *widget, gpointer userdata) {
    App *app = userdata;
    if (app->delivery_page == widget) {
        app->delivery_page = NULL;
        app->delivery_label = NULL;
    }
}

static void delivery_progress(App *app, const gchar *message) {
    if (app->delivery_label) gtk_label_set_text(GTK_LABEL(app->delivery_label), message);
    set_status(app, message);
}

static void show_delivery_page(App *app) {
    GtkWidget *body, *actions, *close_button;
    GtkWidget *page = new_kindle_page(app, "Доставка книги", &body, &actions);
    if (!page) return;
    app->delivery_page = page;
    app->delivery_label = gtk_label_new("Книга скачивается…");
    set_large_font(app->delivery_label, "Sans 22");
    gtk_label_set_line_wrap(GTK_LABEL(app->delivery_label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(app->delivery_label), 0, 0);
    gtk_box_pack_start(GTK_BOX(body), app->delivery_label, FALSE, FALSE, 12);
    close_button = page_button("Вернуться к книгам");
    gtk_box_pack_start(GTK_BOX(actions), close_button, TRUE, TRUE, 0);
    g_signal_connect_swapped(close_button, "clicked", G_CALLBACK(gtk_widget_destroy), page);
    g_signal_connect(page, "destroy", G_CALLBACK(delivery_page_destroyed), app);
    gtk_widget_show_all(page);
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
        copy_common_task_fields(task, app);
        task->book_id = g_strdup(page->book->id);
        task->title = g_strdup(page->book->title);
        gtk_widget_destroy(page->window);
        show_delivery_page(app);
        delivery_progress(app, "Книга скачивается…");
        if (!start_async_task(task)) delivery_progress(app, "Не удалось запустить скачивание. Повторите попытку.");
    }
}

static void details_page_favorite(GtkButton *button, gpointer userdata) {
    DetailsPage *page = userdata;
    App *app = page->app;
    gboolean selected = !bookrelay_favorites_contains(app->favorites, page->book->id);
    GError *error = NULL;
    if (!bookrelay_favorites_set(app->favorites, page->book, selected, &error)) {
        show_error(app, "Не удалось сохранить избранное", error);
        g_clear_error(&error);
        return;
    }
    set_icon_image(page->favorite_image, selected ? "star-filled.png" : "star.png");
    gtk_widget_set_tooltip_text(page->favorite_button,
                                selected ? "Убрать из избранного" : "Добавить в избранное");
    set_status(app, selected ? "Книга добавлена в избранное" : "Книга удалена из избранного");
    if (app->view == VIEW_FAVORITES) {
        guint total = MAX(1, (app->favorites->books->len + items_per_page(app) - 1) / items_per_page(app));
        app->page = MIN(app->page, total);
        render_books(app, app->favorites->books);
    }
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
    GtkWidget *favorite_button;
    GtkWidget *send_group;
    DetailsPage *page;
    gchar *text;

    window = new_kindle_page(row->app, "Карточка книги", &body, &actions);
    if (!window) return;
    page = g_new0(DetailsPage, 1);
    page->app = row->app;
    page->window = window;
    page->book = bookrelay_book_copy(row->book);
    g_object_set_data_full(G_OBJECT(window), "bookrelay-details-page", page, (GDestroyNotify)details_page_free);

    cover = make_cover(row->app, page->book, 160);
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
    favorite_button = gtk_button_new();
    page->favorite_button = favorite_button;
    page->favorite_image = ink_icon_image(bookrelay_favorites_contains(row->app->favorites, page->book->id)
                                         ? "star-filled.png" : "star.png");
    gtk_container_add(GTK_CONTAINER(favorite_button), page->favorite_image);
    gtk_widget_set_tooltip_text(favorite_button,
        bookrelay_favorites_contains(row->app->favorites, page->book->id)
        ? "Убрать из избранного" : "Добавить в избранное");
    make_touch_target(favorite_button, 76, 68);
    gtk_button_set_relief(GTK_BUTTON(favorite_button), GTK_RELIEF_NONE);
    send_group = gtk_hbox_new(FALSE, 8);
    g_signal_connect(close_button, "clicked", G_CALLBACK(details_page_close), page);
    g_signal_connect(send_button, "clicked", G_CALLBACK(details_page_send), page);
    g_signal_connect(favorite_button, "clicked", G_CALLBACK(details_page_favorite), page);
    ink_primary_button(send_button);
    gtk_box_pack_start(GTK_BOX(actions), close_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(send_group), send_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(send_group), favorite_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), send_group, TRUE, TRUE, 0);
    gtk_widget_show_all(window);
    g_free(text);
}

static void search_page(App *app, guint page) {
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(app->query));
    AsyncTask *task;
    if (!query || !*query) {
        clear_results(app);
        render_empty_state(app, "Введите запрос");
        set_status(app, "Укажите название или автора");
        gtk_window_set_focus(GTK_WINDOW(app->window), app->query);
        return;
    }
    if (!app->config->token || !*app->config->token) {
        set_status(app, "Подключите relay в настройках для поиска книг");
        return;
    }
    if (app->keyboard)
        virtual_keyboard_hide(g_object_get_data(G_OBJECT(app->keyboard), "bookrelay-keyboard-state"));
    set_status(app, "Ищем книги…");
    app->view = VIEW_SEARCH;
    app->page = page;
    app->has_next = FALSE;
    advance_generation(app);
    gtk_widget_hide(app->header_title);
    gtk_widget_show(app->search_row);
    gtk_label_set_text(GTK_LABEL(app->section_title), "Результаты поиска");
    update_pager(app);
    task = async_task_new(app, TASK_SEARCH);
    copy_common_task_fields(task, app);
    task->query = g_strdup(query ? query : "");
    task->page = page;
    task->size = items_per_page(app);
    start_async_task(task);
}

static void search_entry_activate(GtkEntry *entry, gpointer userdata) { search_page((App *)userdata, 1); }

static void previous_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (app->page > 1) navigate_view(app, app->view, app->page - 1);
}

static void next_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (gtk_widget_get_sensitive(app->next_page)) navigate_view(app, app->view, app->page + 1);
}

static void first_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    navigate_view(app, app->view, 1);
}

static void last_page_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    guint count = app->view == VIEW_CATEGORIES ? app->catalog_categories->len :
                  app->view == VIEW_SUBCATEGORIES && app->subcategories ? app->subcategories->len :
                  app->view == VIEW_FAVORITES && app->favorites ? app->favorites->books->len : 0;
    if (count) navigate_view(app, app->view, MAX(1, (count + items_per_page(app) - 1) / items_per_page(app)));
}

static void home_clicked(GtkButton *button, gpointer userdata) {
    navigate_view((App *)userdata, VIEW_CATEGORIES, 1);
}

static void favorites_clicked(GtkButton *button, gpointer userdata) {
    navigate_view((App *)userdata, VIEW_FAVORITES, 1);
}

static void breadcrumb_category_clicked(GtkButton *button, gpointer userdata) {
    navigate_view((App *)userdata, VIEW_SUBCATEGORIES, 1);
}

static void update_breadcrumbs(App *app) {
    gchar *category;
    if (app->view != VIEW_SUBCATEGORIES && app->view != VIEW_BOOKS) {
        gtk_widget_hide(app->breadcrumb_row);
        return;
    }
    category = list_excerpt(app->category_title ? app->category_title : "Категория", app->columns == 4 ? 40 : 22);
    gtk_button_set_label(GTK_BUTTON(app->breadcrumb_category), category);
    gtk_widget_set_tooltip_text(app->breadcrumb_category, app->category_title);
    g_free(category);
    if (app->view == VIEW_BOOKS) {
        gchar *current = list_excerpt(app->subcategory_title ? app->subcategory_title : "Подкатегория", app->columns == 4 ? 40 : 22);
        gtk_label_set_text(GTK_LABEL(app->breadcrumb_current), current);
        gtk_widget_set_tooltip_text(app->breadcrumb_current, app->subcategory_title);
        g_free(current);
        gtk_widget_show(app->breadcrumb_separator);
        gtk_widget_show(app->breadcrumb_current);
    } else {
        gtk_widget_hide(app->breadcrumb_separator);
        gtk_widget_hide(app->breadcrumb_current);
    }
    gtk_widget_show(app->breadcrumb_row);
}

static void navigate_view(App *app, guint view, guint page) {
    AsyncTask *task;
    app->view = view;
    app->page = page;
    app->has_next = FALSE;
    advance_generation(app);
    gtk_widget_hide(app->search_row);
    gtk_widget_show(app->header_title);
    if (app->keyboard)
        virtual_keyboard_hide(g_object_get_data(G_OBJECT(app->keyboard), "bookrelay-keyboard-state"));
    update_breadcrumbs(app);
    if (view == VIEW_CATEGORIES) gtk_label_set_text(GTK_LABEL(app->section_title), "Категории");
    else if (view == VIEW_SUBCATEGORIES) gtk_label_set_text(GTK_LABEL(app->section_title), app->category_title);
    else if (view == VIEW_BOOKS) gtk_label_set_text(GTK_LABEL(app->section_title), app->subcategory_title);
    else if (view == VIEW_FAVORITES) gtk_label_set_text(GTK_LABEL(app->section_title), "Избранное");
    if (view == VIEW_SEARCH) { search_page(app, page); return; }
    if (view == VIEW_CATEGORIES) { render_categories(app); return; }
    if (view == VIEW_FAVORITES) { render_books(app, app->favorites->books); return; }
    if (view == VIEW_SUBCATEGORIES && app->subcategories) { render_subcategories(app); return; }
    clear_results(app);
    render_empty_state(app, view == VIEW_SUBCATEGORIES ? "Загружаем подкатегории…" : "Загружаем книги…");
    update_pager(app);
    task = async_task_new(app, view == VIEW_SUBCATEGORIES ? TASK_SUBCATEGORIES : TASK_CATALOG_BOOKS);
    copy_common_task_fields(task, app);
    task->category = g_strdup(app->category_id);
    task->subcategory = g_strdup(app->subcategory_id);
    task->page = page;
    task->size = items_per_page(app);
    start_async_task(task);
}

static gchar *normalize_relay_url(const gchar *value) {
    gchar *url = g_strdup(value ? value : "");
    gchar *end;
    g_strstrip(url);
    end = url + strlen(url);
    while (end > url && end[-1] == '/') *--end = '\0';
    if (*url && !strstr(url, "://")) {
        gchar *https_url = g_strconcat("https://", url, NULL);
        g_free(url);
        url = https_url;
    }
    return url;
}

static gboolean valid_relay_url(const gchar *value) {
    const gchar *host;
    const gchar *cursor;
    if (!value || (!g_str_has_prefix(value, "https://") && !g_str_has_prefix(value, "http://"))) return FALSE;
    host = strstr(value, "://") + 3;
    if (!*host || *host == '/' || *host == ':') return FALSE;
    for (cursor = value; *cursor; cursor++)
        if (g_ascii_isspace(*cursor)) return FALSE;
    return TRUE;
}

static const gchar *display_relay_url(const gchar *value) {
    return value && g_str_has_prefix(value, "https://") ? value + 8 : (value ? value : "");
}

static gboolean load_categories(App *app) {
    AsyncTask *task;
    if (!app->config->token || !*app->config->token) return FALSE;
    task = async_task_new(app, TASK_CATEGORIES);
    copy_common_task_fields(task, app);
    return start_async_task(task);
}

static void retry_categories_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (app->catalog_ready || !app->page_window) return;
    SettingsPage *page = g_object_get_data(G_OBJECT(app->page_window), "bookrelay-settings-page");
    if (page) gtk_label_set_text(GTK_LABEL(page->feedback), "Загружаем категории…");
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    if (!load_categories(app)) {
        if (page) gtk_label_set_text(GTK_LABEL(page->feedback), "Не удалось начать загрузку каталога. Повторите попытку.");
        gtk_widget_set_sensitive(GTK_WIDGET(button), TRUE);
    }
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
    if (!start_async_task(task)) poll->request_pending = FALSE;
    return TRUE;
}

static void settings_page_cancel(GtkButton *button, gpointer userdata) {
    SettingsPage *page = userdata;
    if (!page->app->catalog_ready) {
        exit_clicked(NULL, page->app);
        return;
    }
    gtk_widget_destroy(page->window);
}

static void settings_page_connect(GtkButton *button, gpointer userdata) {
    SettingsPage *page = userdata;
    App *app = page->app;
    GtkWidget *connect = g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect");
    gchar *relay_url = normalize_relay_url(gtk_entry_get_text(page->relay));
    gchar *email = g_strdup(gtk_entry_get_text(page->email));
    gchar *code = g_ascii_strup(gtk_entry_get_text(page->code), -1);
    AsyncTask *task;
    GError *save_error = NULL;
    gboolean reuse_token;
    g_strstrip(email);
    g_strstrip(code);
    if (!GTK_WIDGET_IS_SENSITIVE(connect)) goto done;
    reuse_token = page->claimed && !*code && app->config->token && *app->config->token &&
                  g_strcmp0(relay_url, app->config->relay_url) == 0;
    if (!valid_relay_url(relay_url)) {
        gtk_label_set_text(GTK_LABEL(page->feedback), "Введите адрес сервера, например book.example.com");
        gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->relay));
        virtual_keyboard_show_for(page->keyboard, page->relay);
    } else if (!strchr(email, '@') || strchr(email, ' ') || !strchr(strchr(email, '@'), '.')) {
        gtk_label_set_text(GTK_LABEL(page->feedback), "Введите корректную почту Kindle");
        gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->email));
        virtual_keyboard_show_for(page->keyboard, page->email);
    } else if (!reuse_token && (strlen(code) != 8 || strspn(code, "0123456789ABCDEF") != 8)) {
        gtk_label_set_text(GTK_LABEL(page->feedback), "Введите 8 символов одноразового кода");
        gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->code));
        virtual_keyboard_show_for(page->keyboard, page->code);
    } else {
        if (reuse_token && !bookrelay_config_save(app->config, app->config_path, &save_error)) {
            gchar *message = g_strdup_printf("Код принят, но токен не сохранён на Kindle: %s. Проверьте свободное место и повторите без кода.",
                                             save_error ? save_error->message : "ошибка записи");
            gtk_label_set_text(GTK_LABEL(page->feedback), message);
            g_free(message);
            g_clear_error(&save_error);
            goto done;
        }
        task = async_task_new(app, reuse_token ? TASK_UPDATE_EMAIL : TASK_PAIR_CLAIM);
        task->base_url = g_strdup(relay_url);
        task->code = g_strdup(code);
        task->email = g_strdup(email);
        if (task->kind == TASK_UPDATE_EMAIL) task->token = g_strdup(app->config->token);
        task->settings_page = page->window;
        gtk_label_set_text(GTK_LABEL(page->feedback), "Подключаем Kindle…");
        if (start_async_task(task)) gtk_widget_set_sensitive(connect, FALSE);
        else gtk_label_set_text(GTK_LABEL(page->feedback), "Не удалось запустить подключение. Повторите попытку.");
    }
done:
    g_free(relay_url); g_free(email); g_free(code);
}

static void settings_code_activate(GtkEntry *entry, gpointer userdata) {
    settings_page_connect(NULL, userdata);
}

static void settings_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *body;
    GtkWidget *actions;
    GtkWidget *window;
    GtkWidget *relay_label;
    GtkWidget *email_label;
    GtkWidget *hint;
    GtkWidget *code_label;
    GtkWidget *cancel_button;
    GtkWidget *connect_button;
    GtkWidget *retry_button;
    SettingsPage *page;

    window = new_kindle_page(app, "Настройки", &body, &actions);
    if (!window) return;
    page = g_new0(SettingsPage, 1);
    page->app = app;
    page->window = window;
    page->relay = GTK_ENTRY(gtk_entry_new());
    page->email = GTK_ENTRY(gtk_entry_new());
    page->code = GTK_ENTRY(gtk_entry_new());
    page->claimed = app->config->token && *app->config->token;
    page->feedback = gtk_label_new("");
    g_object_set_data_full(G_OBJECT(window), "bookrelay-settings-page", page, g_free);

    relay_label = gtk_label_new("Адрес сервера");
    email_label = gtk_label_new("Почта Kindle");
    code_label = gtk_label_new("Код подключения");
    hint = gtk_label_new("Создайте код на странице /pair вашего сервера. Если устройство уже подключено, оставьте код пустым, чтобы обновить почту.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(hint), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(relay_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(email_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(code_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(page->feedback), 0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(page->feedback), TRUE);
    set_large_font(relay_label, "Sans 21");
    set_large_font(email_label, "Sans 21");
    set_large_font(code_label, "Sans 21");
    set_large_font(page->feedback, "Sans 19");
    set_large_font(hint, "Sans 20");
    set_large_font(GTK_WIDGET(page->relay), "Sans 25");
    set_large_font(GTK_WIDGET(page->email), "Sans 25");
    set_large_font(GTK_WIDGET(page->code), "Sans 25");
    gtk_widget_set_size_request(GTK_WIDGET(page->relay), -1, 70);
    gtk_widget_set_size_request(GTK_WIDGET(page->email), -1, 70);
    gtk_widget_set_size_request(GTK_WIDGET(page->code), -1, 70);
    gtk_entry_set_width_chars(page->relay, 8);
    gtk_entry_set_width_chars(page->email, 8);
    gtk_entry_set_width_chars(page->code, 8);
    gtk_entry_set_max_length(page->code, 8);
    gtk_entry_set_text(page->relay, display_relay_url(app->config->relay_url));
    gtk_entry_set_text(page->email, app->config->kindle_email ? app->config->kindle_email : "");
    gtk_box_pack_start(GTK_BOX(body), relay_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->relay), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), email_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->email), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), code_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), GTK_WIDGET(page->code), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), page->feedback, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(body), hint, FALSE, FALSE, 4);
    page->keyboard = virtual_keyboard_new(NULL, page->relay);
    virtual_keyboard_bind(page->keyboard, page->email);
    virtual_keyboard_bind(page->keyboard, page->code);
    g_signal_connect(page->code, "activate", G_CALLBACK(settings_code_activate), page);
    attach_page_keyboard(window, page->keyboard);

    cancel_button = page_button(app->catalog_ready ? "Отмена" : "Выход");
    connect_button = page_button("Подключить");
    retry_button = page_button("Повторить загрузку каталога");
    g_object_set_data(G_OBJECT(window), "bookrelay-settings-connect", connect_button);
    g_object_set_data(G_OBJECT(window), "bookrelay-settings-retry", retry_button);
    ink_primary_button(connect_button);
    g_signal_connect(cancel_button, "clicked", G_CALLBACK(settings_page_cancel), page);
    g_signal_connect(connect_button, "clicked", G_CALLBACK(settings_page_connect), page);
    g_signal_connect(retry_button, "clicked", G_CALLBACK(retry_categories_clicked), app);
    gtk_box_pack_start(GTK_BOX(actions), cancel_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), connect_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), retry_button, TRUE, TRUE, 0);
    gtk_widget_show_all(window);
    gtk_widget_hide(retry_button);
    virtual_keyboard_show_for(page->keyboard, page->relay);
    gtk_window_set_focus(GTK_WINDOW(app->window), GTK_WIDGET(page->relay));
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, focus_widget_idle,
                    g_object_ref(page->relay), g_object_unref);
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
            if (task->generation != app->generation || app->view != VIEW_SEARCH) break;
            if (!task->books) {
                show_error(app, "Поиск не выполнен", task->error);
            } else {
                app->page = task->page;
                app->has_next = task->has_next;
                render_books(app, task->books);
                set_status(app, NULL);
            }
            break;
        case TASK_CATEGORIES:
            if (!task->categories) {
                show_error(app, "Категории не загрузились", task->error);
                if (app->page_window) {
                    SettingsPage *page = g_object_get_data(G_OBJECT(app->page_window), "bookrelay-settings-page");
                    if (page) {
                        gchar *message = g_strdup_printf("Категории не загрузились: %s. Проверьте подключение и повторите.",
                                                         task->error ? task->error->message : "relay не ответил");
                        GtkWidget *retry = g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-retry");
                        gtk_label_set_text(GTK_LABEL(page->feedback), message);
                        if (retry) {
                            gtk_widget_show(retry);
                            gtk_widget_set_sensitive(retry, TRUE);
                        }
                        g_free(message);
                    }
                }
            } else {
                gboolean first_load = !app->catalog_ready;
                if (app->catalog_categories) g_ptr_array_free(app->catalog_categories, TRUE);
                app->catalog_categories = task->categories;
                task->categories = NULL;
                app->catalog_ready = TRUE;
                if (app->view == VIEW_CATEGORIES) render_categories(app);
                if (first_load && app->page_window) gtk_widget_destroy(app->page_window);
                set_status(app, NULL);
            }
            break;
        case TASK_SUBCATEGORIES:
            if (task->generation != app->generation || app->view != VIEW_SUBCATEGORIES) break;
            if (!task->categories) {
                clear_results(app);
                render_empty_state(app, "Не удалось загрузить подкатегории");
                update_pager(app);
                show_error(app, "Подкатегории не загрузились", task->error);
            }
            else {
                if (app->subcategories) g_ptr_array_free(app->subcategories, TRUE);
                app->subcategories = task->categories;
                task->categories = NULL;
                render_subcategories(app);
                set_status(app, NULL);
            }
            break;
        case TASK_CATALOG_BOOKS:
            if (task->generation != app->generation || app->view != VIEW_BOOKS) break;
            if (!task->books) {
                clear_results(app);
                render_empty_state(app, "Не удалось загрузить книги");
                update_pager(app);
                show_error(app, "Книги не загрузились", task->error);
            }
            else {
                app->has_next = task->has_next;
                render_books(app, task->books);
                set_status(app, NULL);
            }
            break;
        case TASK_PAIR_CLAIM:
            if (!task->claim) {
                if (app->page_window == task->settings_page) {
                    SettingsPage *page = g_object_get_data(G_OBJECT(task->settings_page), "bookrelay-settings-page");
                    gchar *message = g_strdup_printf("Подключение не выполнено: %s", task->error ? task->error->message : "relay не вернул ответ");
                    gtk_label_set_text(GTK_LABEL(page->feedback), message);
                    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect"), TRUE);
                    g_free(message);
                } else show_error(app, "Подключение не выполнено", task->error);
            } else {
                GError *save_error = NULL;
                g_free(app->config->relay_url);
                app->config->relay_url = g_strdup(task->base_url);
                reload_favorites(app);
                g_free(app->config->token);
                app->config->token = g_strdup(task->claim->token);
                g_free(app->config->kindle_email);
                app->config->kindle_email = g_strdup(task->claim->kindle_email ? task->claim->kindle_email : "");
                if (app->page_window == task->settings_page) {
                    SettingsPage *page = g_object_get_data(G_OBJECT(task->settings_page), "bookrelay-settings-page");
                    page->claimed = TRUE;
                    gtk_entry_set_text(page->code, "");
                }
                if (!bookrelay_config_save(app->config, app->config_path, &save_error)) {
                    if (app->page_window == task->settings_page) {
                        SettingsPage *page = g_object_get_data(G_OBJECT(task->settings_page), "bookrelay-settings-page");
                        gchar *message = g_strdup_printf("Код принят, но токен не сохранён на Kindle: %s. Проверьте свободное место и повторите без кода.",
                                                         save_error ? save_error->message : "ошибка записи");
                        gtk_label_set_text(GTK_LABEL(page->feedback), message);
                        gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect"), TRUE);
                        g_free(message);
                    } else show_error(app, "Код принят, но подключение не сохранено", save_error);
                    g_clear_error(&save_error);
                    break;
                }
                if (app->subcategories) {
                    g_ptr_array_free(app->subcategories, TRUE);
                    app->subcategories = NULL;
                }
                if (app->page_window == task->settings_page) {
                    SettingsPage *page = g_object_get_data(G_OBJECT(task->settings_page), "bookrelay-settings-page");
                    AsyncTask *update = async_task_new(app, TASK_UPDATE_EMAIL);
                    gtk_label_set_text(GTK_LABEL(page->feedback), "Код принят. Сохраняем почту Kindle…");
                    update->base_url = g_strdup(task->base_url);
                    update->token = g_strdup(task->claim->token);
                    update->email = g_strdup(task->email);
                    update->settings_page = page->window;
                    if (!start_async_task(update)) {
                        gtk_label_set_text(GTK_LABEL(page->feedback), "Код принят, но не удалось запустить сохранение почты. Нажмите «Подключить» ещё раз.");
                        gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect"), TRUE);
                    }
                } else load_categories(app);
            }
            break;
        case TASK_UPDATE_EMAIL:
            if (!task->state || !*task->state) {
                if (app->page_window == task->settings_page) {
                    SettingsPage *page = g_object_get_data(G_OBJECT(task->settings_page), "bookrelay-settings-page");
                    gchar *message = g_strdup_printf("Код принят, но почта не сохранена: %s. Повторите подключение без кода.", task->error ? task->error->message : "неожиданный ответ relay");
                    gtk_label_set_text(GTK_LABEL(page->feedback), message);
                    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect"), TRUE);
                    g_free(message);
                } else show_error(app, "Почта Kindle не сохранена", task->error);
            } else {
                g_free(app->config->relay_url);
                app->config->relay_url = g_strdup(task->base_url);
                reload_favorites(app);
                g_free(app->config->kindle_email);
                app->config->kindle_email = g_strdup(task->state);
                if (!bookrelay_config_save(app->config, app->config_path, &task->error)) {
                    if (app->page_window == task->settings_page) {
                        SettingsPage *page = g_object_get_data(G_OBJECT(task->settings_page), "bookrelay-settings-page");
                        gtk_label_set_text(GTK_LABEL(page->feedback), task->error->message);
                        gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect"), TRUE);
                    }
                } else {
                    app->catalog_ready = FALSE;
                    set_status(app, "Kindle привязан · загружаем каталог…");
                    load_categories(app);
                }
            }
            break;
        case TASK_SEND:
            if (!task->job_id) {
                gchar *message = g_strdup_printf("Не удалось начать скачивание: %s", task->error ? task->error->message : "relay не ответил");
                delivery_progress(app, message);
                g_free(message);
            } else {
                DeliveryPoll *poll = g_new0(DeliveryPoll, 1);
                poll->app = app;
                poll->job_id = g_strdup(task->job_id);
                poll->source_id = g_timeout_add_seconds(3, poll_delivery, poll);
                delivery_progress(app, "Книга скачивается и готовится к отправке…");
            }
            break;
        case TASK_DELIVERY_STATUS:
            if (task->delivery_poll) {
                DeliveryPoll *poll = task->delivery_poll;
                poll->request_pending = FALSE;
                poll->attempts++;
                if (!task->state && poll->attempts < 20) break;
                if (!task->state) delivery_progress(app, "Не удалось получить статус доставки. Проверьте relay.");
                else if (g_strcmp0(task->state, "sent") == 0) delivery_progress(app, "EPUB принят почтовым сервером. Если книга не появилась на Kindle, проверьте разрешённый адрес отправителя в Amazon и почту Kindle в настройках.");
                else if (g_strcmp0(task->state, "failed") == 0) {
                    gchar *message = g_strdup_printf("Не удалось отправить EPUB: %s", task->error ? task->error->message : "проверьте задание на сервере");
                    delivery_progress(app, message);
                    g_free(message);
                }
                else if (poll->attempts < 20) {
                    delivery_progress(app, g_strcmp0(task->state, "sending") == 0 ? "EPUB отправляется…" : "Книга скачивается…");
                    break;
                } else delivery_progress(app, "Задание всё ещё выполняется. Проверьте его на сервере позже.");
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
                if (pixbuf) {
                    /* Do not cache an HTTP response that GTK cannot decode. */
                    if (cache_has_room(directory, path, task->cover_bytes->len))
                        g_file_set_contents(path, (const gchar *)task->cover_bytes->data,
                                            (gssize)task->cover_bytes->len, NULL);
                    if (task->generation == app->generation) {
                        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, task->cover_width,
                                                                     task->cover_height, GDK_INTERP_BILINEAR);
                        gtk_image_set_from_pixbuf(GTK_IMAGE(task->image), scaled);
                        gtk_widget_show(task->image);
                        if (task->placeholder) gtk_widget_hide(task->placeholder);
                        g_object_unref(scaled);
                    }
                    g_object_unref(pixbuf);
                }
                g_free(directory);
                g_free(path);
            }
            break;
    }
    if (task->kind == TASK_COVER && app->pending_covers > 0) app->pending_covers--;
    if (app->active_tasks > 0) app->active_tasks--;
    async_task_free(task);
    start_deferred_covers(app);
    return FALSE;
}

static void search_icon_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    if (!app->catalog_ready) {
        show_setup_if_needed(app);
        return;
    }
    if (GTK_WIDGET_VISIBLE(app->search_row) && *gtk_entry_get_text(GTK_ENTRY(app->query))) {
        search_page(app, 1);
        return;
    }
    app->view = VIEW_SEARCH;
    app->page = 1;
    app->has_next = FALSE;
    advance_generation(app);
    ((VirtualKeyboard *)g_object_get_data(G_OBJECT(app->keyboard),
                                          "bookrelay-keyboard-state"))->defer_search_open = TRUE;
    gtk_label_set_text(GTK_LABEL(app->section_title), "Поиск книг");
    gtk_widget_hide(app->breadcrumb_row);
    clear_results(app);
    render_empty_state(app, "Введите запрос");
    update_pager(app);
    gtk_widget_hide(app->header_title);
    gtk_widget_show(app->search_row);
    /* Opening LIPC while GTK is still handling the icon release can give
     * the keyboard X focus before the newly shown entry is ready for input. */
    g_idle_add(focus_search_idle, app);
}

static void help_clicked(GtkButton *button, gpointer userdata) {
    App *app = userdata;
    GtkWidget *body, *actions, *page, *label, *close_button;
    page = new_kindle_page(app, "Как пользоваться", &body, &actions);
    if (!page) return;
    label = gtk_label_new("1. Подключите relay в настройках: укажите адрес сервера и одноразовый код.\n\n2. Выберите категорию и подкатегорию или найдите книгу через поиск.\n\n3. Нажмите на обложку книги и выберите «Скачать на Kindle». EPUB будет отправлен на адрес вашего Kindle.\n\nЗвезда на странице книги добавляет её в избранное или убирает оттуда. Звезда в верхней панели открывает избранное. Иконка домика возвращает на главную. Стрелки внизу перелистывают страницы.");
    set_large_font(label, "Sans 22");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
    gtk_box_pack_start(GTK_BOX(body), label, FALSE, FALSE, 12);
    close_button = page_button("Закрыть");
    g_signal_connect_swapped(close_button, "clicked", G_CALLBACK(gtk_widget_destroy), page);
    gtk_box_pack_start(GTK_BOX(actions), close_button, TRUE, TRUE, 0);
    gtk_widget_show_all(page);
}

static GtkWidget *ink_icon_image(const gchar *filename) {
    const gchar *asset_dir = g_getenv("BOOKRELAY_ASSET_DIR");
    gchar *path = g_build_filename(asset_dir && *asset_dir ? asset_dir : "client/share/covers", "..", "icons", filename, NULL);
    GtkWidget *icon = gtk_image_new_from_file(path);
    g_free(path);
    return icon;
}

static void set_icon_image(GtkWidget *image, const gchar *filename) {
    const gchar *asset_dir = g_getenv("BOOKRELAY_ASSET_DIR");
    gchar *path = g_build_filename(asset_dir && *asset_dir ? asset_dir : "client/share/covers", "..", "icons", filename, NULL);
    gtk_image_set_from_file(GTK_IMAGE(image), path);
    g_free(path);
}

static GtkWidget *header_icon(const gchar *filename, const gchar *tooltip) {
    GtkWidget *surface = gtk_event_box_new();
    GtkWidget *icon = ink_icon_image(filename);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(surface), FALSE);
    gtk_event_box_set_above_child(GTK_EVENT_BOX(surface), TRUE);
    gtk_container_add(GTK_CONTAINER(surface), icon);
    gtk_widget_set_tooltip_text(surface, tooltip);
    make_touch_target(surface, gdk_screen_get_width(gdk_screen_get_default()) < 1000 ? 64 : 76,
                      gdk_screen_get_width(gdk_screen_get_default()) < 1000 ? 64 : 76);
    return surface;
}

static gboolean home_icon_pressed(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    if (event->button == 1 && ((App *)userdata)->catalog_ready) home_clicked(NULL, userdata);
    return TRUE;
}

static gboolean search_icon_press_timeout(gpointer userdata) {
    App *app = userdata;
    app->search_press_timeout = 0;
    app->search_press_handled = TRUE;
    search_icon_clicked(NULL, app);
    return FALSE;
}

static gboolean search_icon_pressed(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    App *app = userdata;
    if (event->button != 1) return FALSE;
    if (app->search_press_timeout) g_source_remove(app->search_press_timeout);
    app->search_press_handled = FALSE;
    /* Kindle touch input may omit button-release. Give a normal tap time to
     * finish before falling back to the press event. */
    app->search_press_timeout = g_timeout_add(250, search_icon_press_timeout, app);
    return TRUE;
}

static gboolean search_icon_released(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    App *app = userdata;
    if (event->button != 1) return FALSE;
    if (app->search_press_timeout) {
        g_source_remove(app->search_press_timeout);
        app->search_press_timeout = 0;
    }
    if (!app->search_press_handled) {
        app->search_press_handled = TRUE;
        search_icon_clicked(NULL, app);
    }
    return TRUE;
}

static void search_icon_window_destroyed(GtkWidget *widget, gpointer userdata) {
    App *app = userdata;
    if (app->search_press_timeout) {
        g_source_remove(app->search_press_timeout);
        app->search_press_timeout = 0;
    }
}

static gboolean search_window_key_press(GtkWidget *widget, GdkEventKey *event, gpointer userdata) {
    App *app = userdata;
    gint cursor;
    /* The Kindle keyboard sometimes restores focus to the application window
     * after opening. Keep its keystrokes attached to the visible search field. */
    if (app->page_window || app->view != VIEW_SEARCH ||
        !GTK_WIDGET_MAPPED(app->query) ||
        gtk_window_get_focus(GTK_WINDOW(widget)) == app->query)
        return FALSE;
    cursor = gtk_editable_get_position(GTK_EDITABLE(app->query));
    gtk_widget_grab_focus(app->query);
    gtk_editable_select_region(GTK_EDITABLE(app->query), cursor, cursor);
    /* Route the key to the search entry after Kindle restores focus to the
     * application window. Stop window dispatch once the entry handles it. */
    return gtk_widget_event(app->query, (GdkEvent *)event);
}

static gboolean search_window_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer userdata) {
    App *app = userdata;
    VirtualKeyboard *keyboard = g_object_get_data(G_OBJECT(app->keyboard), "bookrelay-keyboard-state");
    /* Input methods send composed text to the focused entry, not necessarily
     * as key presses. Restore that focus when Kindle returns to our window. */
    if (!app->page_window && app->view == VIEW_SEARCH &&
        GTK_WIDGET_MAPPED(app->query) && keyboard->native_open) {
        /* A returned X focus does not imply the entry's IM context has focus.
         * GTK can still report this entry as focused after the overlay left. */
        refocus_search_entry(app);
    }
    return FALSE;
}

static gboolean favorites_icon_pressed(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    if (event->button == 1 && ((App *)userdata)->catalog_ready) favorites_clicked(NULL, userdata);
    return TRUE;
}

static gboolean settings_icon_pressed(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    if (event->button == 1) settings_clicked(NULL, userdata);
    return TRUE;
}

static gboolean help_icon_pressed(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    if (event->button == 1) help_clicked(NULL, userdata);
    return TRUE;
}

static gboolean exit_icon_pressed(GtkWidget *widget, GdkEventButton *event, gpointer userdata) {
    if (event->button == 1) exit_clicked(NULL, userdata);
    return TRUE;
}

static void build_ui(App *app) {
    VirtualKeyboard *main_keyboard;
    GtkWidget *root = gtk_vbox_new(FALSE, 8);
    GtkWidget *shell = gtk_vbox_new(FALSE, 0);
    GtkWidget *header = gtk_hbox_new(FALSE, gdk_screen_get_width(gdk_screen_get_default()) < 1000 ? 4 : 10);
    GtkWidget *title = gtk_label_new("Books Store");
    GtkWidget *home = header_icon("home.png", "На главную: категории");
    GtkWidget *section = gtk_vbox_new(FALSE, 2);
    GtkWidget *breadcrumbs = gtk_hbox_new(FALSE, 2);
    GtkWidget *breadcrumb_home = gtk_button_new_with_label("Категории");
    GtkWidget *breadcrumb_divider = gtk_label_new("›");
    GtkWidget *breadcrumb_category = gtk_button_new_with_label("");
    GtkWidget *breadcrumb_separator = gtk_label_new("›");
    GtkWidget *breadcrumb_current = gtk_label_new("");
    GtkWidget *search_icon = header_icon("search.png", "Поиск книг");
    GtkWidget *favorites_icon = header_icon("star.png", "Избранное");
    GtkWidget *settings_icon = header_icon("settings.png", "Настройки и подключение");
    GtkWidget *help_icon = header_icon("help.png", "Справка");
    GtkWidget *exit_icon = header_icon("close.png", "Выход");
    GtkWidget *navigation = gtk_hbox_new(FALSE, 4);
    GtkWidget *search_hint = gtk_label_new("НАЗВАНИЕ ИЛИ АВТОР");
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    app->pages = gtk_notebook_new();
    app->query = gtk_entry_new();
    app->search_row = gtk_vbox_new(FALSE, 0);
    app->search_icon = search_icon;
    app->header_title = title;
    app->home_button = home;
    app->breadcrumb_row = breadcrumbs;
    app->breadcrumb_category = breadcrumb_category;
    app->breadcrumb_current = breadcrumb_current;
    app->breadcrumb_separator = breadcrumb_separator;
    app->results = gtk_vbox_new(FALSE, 4);
    app->section_title = gtk_label_new("Категории");
    app->status = gtk_label_new("");
    app->first_page = gtk_button_new();
    app->previous_page = gtk_button_new();
    app->next_page = gtk_button_new();
    app->last_page = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(app->first_page), ink_icon_image("first.png"));
    gtk_container_add(GTK_CONTAINER(app->previous_page), ink_icon_image("previous.png"));
    gtk_container_add(GTK_CONTAINER(app->next_page), ink_icon_image("next.png"));
    gtk_container_add(GTK_CONTAINER(app->last_page), ink_icon_image("last.png"));
    gtk_widget_set_tooltip_text(app->first_page, "Первая страница");
    gtk_widget_set_tooltip_text(app->previous_page, "Предыдущая страница");
    gtk_widget_set_tooltip_text(app->next_page, "Следующая страница");
    gtk_widget_set_tooltip_text(app->last_page, "Последняя страница");
    app->page_label = gtk_label_new("Страница 1 из 2");
    app->page = 1;
    app->columns = 4;
    app->view = VIEW_CATEGORIES;
    seed_categories(app);

    gtk_window_set_title(GTK_WINDOW(app->window), KINDLE_APP_WINDOW_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(app->window),
                                gdk_screen_get_width(gdk_screen_get_default()),
                                gdk_screen_get_height(gdk_screen_get_default()));
    ink_background(app->window, "#ffffff");
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(app->pages), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(app->pages), FALSE);
    set_large_font(title, "Sans Bold 30");
    set_large_font(app->section_title, "Sans Bold 22");
    set_large_font(breadcrumb_home, "Sans 13");
    set_large_font(breadcrumb_category, "Sans 13");
    set_large_font(breadcrumb_divider, "Sans 16");
    set_large_font(breadcrumb_separator, "Sans 16");
    set_large_font(breadcrumb_current, "Sans 13");
    ink_text(breadcrumb_divider, "#777777");
    ink_text(breadcrumb_separator, "#777777");
    ink_text(breadcrumb_current, "#555555");
    gtk_button_set_relief(GTK_BUTTON(breadcrumb_home), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(breadcrumb_category), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(breadcrumb_home, FALSE);
    gtk_widget_set_can_focus(breadcrumb_category, FALSE);
    set_large_font(app->query, "Sans 20");
    set_large_font(search_hint, "Sans 11");
    ink_text(search_hint, "#666666");
    gtk_misc_set_alignment(GTK_MISC(search_hint), 0, 0.5);
    set_large_font(app->page_label, "Sans 18");
    set_large_font(app->status, "Sans 14");
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(app->section_title), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(app->status), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(app->section_title), PANGO_ELLIPSIZE_END);
    gtk_entry_set_activates_default(GTK_ENTRY(app->query), TRUE);
    gtk_entry_set_has_frame(GTK_ENTRY(app->query), FALSE);
    gtk_widget_set_size_request(app->query, -1, 48);
    gtk_widget_set_tooltip_text(app->query, "Название или автор");
    make_touch_target(app->first_page, 92, 68);
    make_touch_target(app->previous_page, 92, 68);
    make_touch_target(app->next_page, 92, 68);
    make_touch_target(app->last_page, 92, 68);
    gtk_button_set_relief(GTK_BUTTON(app->first_page), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(app->previous_page), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(app->next_page), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(app->last_page), GTK_RELIEF_NONE);

    gtk_box_pack_start(GTK_BOX(header), home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 6);
    gtk_box_pack_start(GTK_BOX(header), app->search_row, TRUE, TRUE, 6);
    gtk_box_pack_end(GTK_BOX(header), exit_icon, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), help_icon, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), settings_icon, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), favorites_icon, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), search_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(breadcrumbs), breadcrumb_home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(breadcrumbs), breadcrumb_divider, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(breadcrumbs), breadcrumb_category, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(breadcrumbs), breadcrumb_separator, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(breadcrumbs), breadcrumb_current, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(section), breadcrumbs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(section), app->section_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app->search_row), search_hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app->search_row), app->query, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(app->search_row), gtk_hseparator_new(), FALSE, FALSE, 0);
    gtk_widget_show_all(app->search_row);
    gtk_widget_hide(app->search_row);
    gtk_widget_set_no_show_all(app->search_row, TRUE);
    gtk_box_pack_start(GTK_BOX(navigation), app->first_page, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), app->previous_page, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), app->page_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), app->next_page, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(navigation), app->last_page, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), gtk_hseparator_new(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), section, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), app->results, TRUE, TRUE, 0);
    main_keyboard = virtual_keyboard_new(root, GTK_ENTRY(app->query));
    app->keyboard = main_keyboard->root;
    main_keyboard->bottom_widget = navigation;
    gtk_box_pack_end(GTK_BOX(root), navigation, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(root), gtk_hseparator_new(), FALSE, FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(root), 18);
    gtk_box_pack_start(GTK_BOX(shell), app->pages, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(shell), app->status, FALSE, FALSE, 3);
    gtk_widget_set_no_show_all(app->status, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(app->pages), root, NULL);
    gtk_container_add(GTK_CONTAINER(app->window), shell);
    g_signal_connect(search_icon, "button-press-event", G_CALLBACK(search_icon_pressed), app);
    g_signal_connect(search_icon, "button-release-event", G_CALLBACK(search_icon_released), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(search_icon_window_destroyed), app);
    g_signal_connect(favorites_icon, "button-press-event", G_CALLBACK(favorites_icon_pressed), app);
    g_signal_connect(settings_icon, "button-press-event", G_CALLBACK(settings_icon_pressed), app);
    g_signal_connect(help_icon, "button-press-event", G_CALLBACK(help_icon_pressed), app);
    g_signal_connect(exit_icon, "button-press-event", G_CALLBACK(exit_icon_pressed), app);
    g_signal_connect(home, "button-press-event", G_CALLBACK(home_icon_pressed), app);
    g_signal_connect(breadcrumb_home, "clicked", G_CALLBACK(home_clicked), app);
    g_signal_connect(breadcrumb_category, "clicked", G_CALLBACK(breadcrumb_category_clicked), app);
    g_signal_connect(app->query, "activate", G_CALLBACK(search_entry_activate), app);
    g_signal_connect(app->first_page, "clicked", G_CALLBACK(first_page_clicked), app);
    g_signal_connect(app->previous_page, "clicked", G_CALLBACK(previous_page_clicked), app);
    g_signal_connect(app->next_page, "clicked", G_CALLBACK(next_page_clicked), app);
    g_signal_connect(app->last_page, "clicked", G_CALLBACK(last_page_clicked), app);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(delete_event), app);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(search_window_key_press), app);
    g_signal_connect(app->window, "focus-in-event", G_CALLBACK(search_window_focus_in), app);
    gtk_widget_add_events(app->window, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(app->window, "button-press-event", G_CALLBACK(virtual_keyboard_background_press), app);
    gtk_widget_show_all(app->window);
    gtk_widget_hide(app->keyboard);
    gtk_widget_hide(app->search_row);
    gtk_widget_hide(app->breadcrumb_row);
    gtk_window_set_focus(GTK_WINDOW(app->window), NULL);
    settings_clicked(NULL, app);
    if (app->config->token && *app->config->token) load_categories(app);
}

int main(int argc, char **argv) {
    App app = {0};
    const gchar *home = g_get_user_data_dir();
    app.config_path = g_build_filename(home, "bookrelay", "config.ini", NULL);
    app.config = bookrelay_config_load(app.config_path);
    app.favorites = bookrelay_favorites_load(app.config_path, app.config->relay_url);
#if !GLIB_CHECK_VERSION(2, 32, 0)
    g_thread_init(NULL);
#endif
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    build_ui(&app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_main();
    bookrelay_config_free(app.config);
    bookrelay_favorites_free(app.favorites);
    if (app.catalog_categories) g_ptr_array_free(app.catalog_categories, TRUE);
    if (app.subcategories) g_ptr_array_free(app.subcategories, TRUE);
    g_free(app.category_id); g_free(app.category_title);
    g_free(app.subcategory_id); g_free(app.subcategory_title);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
