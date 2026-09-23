/* Run with Xvfb at the Paperwhite resolution. This exercises real GTK widgets,
 * visibility, input and layout; the Python source checks cannot do that. */
#define main bookrelay_application_main
#include "../src/main.c"
#undef main

static void drain_events(void) {
    while (gtk_events_pending()) gtk_main_iteration();
    gdk_display_sync(gdk_display_get_default());
}

static void expect_visible(GtkWidget *widget, const char *name) {
    if (!GTK_WIDGET_VISIBLE(widget) || !GTK_WIDGET_MAPPED(widget))
        g_error("%s is not visible and mapped", name);
}

static void expect_inside_window(App *app, GtkWidget *widget, const char *name) {
    gint x = 0, y = 0;
    gint width, height;
    if (!gtk_widget_translate_coordinates(widget, app->window, 0, 0, &x, &y))
        g_error("%s is outside the active window", name);
    width = app->window->allocation.width;
    height = app->window->allocation.height;
    if (x < 0 || y < 0 || x + widget->allocation.width > width || y + widget->allocation.height > height)
        g_error("%s clipped: %d,%d %dx%d in %dx%d", name, x, y,
                widget->allocation.width, widget->allocation.height, width, height);
}

static GtkWidget *find_button(GtkWidget *root, const gchar *label) {
    GList *children, *item;
    GtkWidget *found = NULL;
    if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0)
        return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    children = gtk_container_get_children(GTK_CONTAINER(root));
    for (item = children; item && !found; item = item->next)
        found = find_button(GTK_WIDGET(item->data), label);
    g_list_free(children);
    return found;
}

static void press_key(GtkWidget *keyboard, const gchar *label) {
    GtkWidget *button = find_button(keyboard, label);
    if (!button) g_error("keyboard has no %s key", label);
    gtk_button_clicked(GTK_BUTTON(button));
}

static void expect_keyboard_labels_fit(GtkWidget *keyboard) {
    GList *rows = gtk_container_get_children(GTK_CONTAINER(keyboard));
    GList *row;
    for (row = rows; row; row = row->next) {
        if (!GTK_IS_BOX(row->data)) continue;
        GList *buttons = gtk_container_get_children(GTK_CONTAINER(row->data));
        GList *item;
        for (item = buttons; item; item = item->next) {
            GtkWidget *button = item->data;
            GtkWidget *label = gtk_bin_get_child(GTK_BIN(button));
            gint width;
            pango_layout_get_pixel_size(gtk_label_get_layout(GTK_LABEL(label)), &width, NULL);
            if (width + 8 > button->allocation.width)
                g_error("keyboard key %s clipped: %d in %d", gtk_button_get_label(GTK_BUTTON(button)),
                        width, button->allocation.width);
        }
        g_list_free(buttons);
    }
    g_list_free(rows);
}

static GtkWidget *find_data_button(GtkWidget *root, const gchar *key) {
    GList *children, *item;
    GtkWidget *found = NULL;
    if (GTK_IS_BUTTON(root) && g_object_get_data(G_OBJECT(root), key)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    children = gtk_container_get_children(GTK_CONTAINER(root));
    for (item = children; item && !found; item = item->next)
        found = find_data_button(GTK_WIDGET(item->data), key);
    g_list_free(children);
    return found;
}

static void snapshot(App *app, const gchar *dir, const char *name) {
    GdkPixbuf *pixels;
    gchar *path = g_build_filename(dir, name, NULL);
    GError *error = NULL;
    drain_events();
    if (app->window->allocation.width != gdk_screen_width() ||
        app->window->allocation.height != gdk_screen_height())
        g_error("%s window exceeds screen: %dx%d instead of %dx%d", name,
                app->window->allocation.width, app->window->allocation.height,
                gdk_screen_width(), gdk_screen_height());
    pixels = gdk_pixbuf_get_from_drawable(NULL, GDK_DRAWABLE(app->window->window),
                                          NULL, 0, 0, 0, 0,
                                          app->window->allocation.width,
                                          app->window->allocation.height);
    if (!pixels || !gdk_pixbuf_save(pixels, path, "png", &error, NULL))
        g_error("snapshot %s: %s", path, error ? error->message : "capture failed");
    g_print("%s %dx%d\n", path, app->window->allocation.width, app->window->allocation.height);
    g_object_unref(pixels);
    g_free(path);
}

int main(int argc, char **argv) {
    App app = {0};
    PairPage *pair;
    SettingsPage *settings;
    VirtualKeyboard *search_keyboard;
    GtkWidget *letter;
    GtkWidget *button;
    GPtrArray *books;
    BookRelayBook *book;
    DetailsPage *details;
    if (argc != 2) g_error("usage: ui-smoke <screenshot-directory>");
    gtk_init(&argc, &argv);
    if (g_getenv("BOOKRELAY_TEST_DPI"))
        gdk_screen_set_resolution(gdk_screen_get_default(), 300.0);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    app.config = g_new0(BookRelayConfig, 1);
    app.config->relay_url = g_strdup("");
    app.config_path = g_build_filename(argv[1], "unused-config.ini", NULL);
    g_mkdir_with_parents(argv[1], 0700);
    build_ui(&app);
    drain_events();
    settings = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-settings-page");
    if (!settings) g_error("setup screen did not open on first launch");
    if (g_getenv("BOOKRELAY_TEST_NATIVE")) {
        gint actions_y, keyboard_y;
        GtkWidget *actions = g_object_get_data(G_OBJECT(settings->window), "bookrelay-page-actions");
        if (!settings->keyboard->native_open)
            g_error("Kindle keyboard open command failed");
        if (!GTK_WIDGET_VISIBLE(settings->keyboard->native_spacer))
            g_error("Kindle keyboard space was not reserved");
        if (!gtk_widget_translate_coordinates(actions, app.window, 0, 0, NULL, &actions_y) ||
            !gtk_widget_translate_coordinates(settings->keyboard->root, app.window, 0, 0, NULL, &keyboard_y) ||
            actions_y >= keyboard_y)
            g_error("settings actions should remain above Kindle keyboard: actions y=%d, keyboard y=%d",
                    actions_y, keyboard_y);
        expect_inside_window(&app, actions, "settings actions with Kindle keyboard");
        snapshot(&app, argv[1], "settings-native.png");
        press_key(settings->keyboard->root, "Клавиатура BookRelay");
        if (!GTK_WIDGET_VISIBLE(g_ptr_array_index(settings->keyboard->letter_buttons, 0)))
            g_error("built-in keyboard fallback did not open");
        if (settings->keyboard->native_open) g_error("Kindle keyboard did not close");
        press_key(settings->keyboard->root, "https://");
        if (g_strcmp0(gtk_entry_get_text(settings->relay), "https://") != 0)
            g_error("keyboard fallback cannot enter HTTPS");
        gtk_widget_destroy(app.window);
        bookrelay_config_free(app.config);
        g_free(app.config_path);
        curl_global_cleanup();
        return 0;
    }
    expect_visible(settings->keyboard->root, "settings keyboard");
    expect_inside_window(&app, GTK_WIDGET(settings->relay), "settings relay field");
    expect_inside_window(&app, settings->keyboard->root, "settings keyboard");
    expect_keyboard_labels_fit(settings->keyboard->root);
    snapshot(&app, argv[1], "settings.png");
    gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(settings->keyboard->letter_buttons, 0)));
    if (g_strcmp0(gtk_entry_get_text(settings->relay), "q") != 0)
        g_error("settings keyboard did not insert text");
    gtk_entry_set_text(settings->relay, "");
    press_key(settings->keyboard->root, "https://");
    {
        const gchar *suffix = "example.org/opds";
        while (*suffix) {
            gchar key[2] = {*suffix++, 0};
            press_key(settings->keyboard->root, key);
        }
    }
    if (g_strcmp0(gtk_entry_get_text(settings->relay), "https://example.org/opds") != 0)
        g_error("keyboard cannot enter a complete HTTPS URL");
    press_key(settings->keyboard->root, "?123");
    press_key(settings->keyboard->root, ":");
    if (g_strcmp0(gtk_entry_get_text(settings->relay), "https://example.org/opds:") != 0)
        g_error("symbols mode did not insert a colon");
    press_key(settings->keyboard->root, "РУС");
    if (!find_button(settings->keyboard->root, "?123"))
        g_error("symbols button was not reset after language switch");
    press_key(settings->keyboard->root, "й");
    if (!g_str_has_suffix(gtk_entry_get_text(settings->relay), ":й"))
        g_error("Russian keyboard inserted the wrong character");

    settings_pair_clicked(NULL, settings);
    drain_events();
    pair = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-pair-page");
    if (!pair) g_error("pairing page not created");
    expect_visible(pair->keyboard->root, "pairing keyboard");
    expect_inside_window(&app, GTK_WIDGET(pair->relay), "relay field");
    expect_inside_window(&app, GTK_WIDGET(pair->code), "code field");
    expect_inside_window(&app, pair->keyboard->root, "pairing keyboard");
    virtual_keyboard_show_for(pair->keyboard, pair->code);
    gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(pair->keyboard->letter_buttons, 0)));
    if (g_strcmp0(gtk_entry_get_text(pair->code), "q") != 0)
        g_error("pairing keyboard did not insert into code field");
    snapshot(&app, argv[1], "pairing.png");

    /* Simulate successful setup without starting a network request. */
    app.config->token = g_strdup("smoke-test-token");
    app.catalog_ready = TRUE;
    gtk_widget_destroy(pair->window);
    drain_events();
    if (app.page_window || gtk_notebook_get_current_page(GTK_NOTEBOOK(app.pages)) != 0)
        g_error("closing pairing did not restore main page");
    render_categories(&app);
    drain_events();
    button = find_data_button(app.results, "category-row");
    if (!button) g_error("category grid has no cards");
    expect_inside_window(&app, button, "category card");
    expect_inside_window(&app, app.page_label, "pagination");
    snapshot(&app, argv[1], "categories.png");

    app.subcategories = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_category_free);
    {
        BookRelayCategory *category = g_new0(BookRelayCategory, 1);
        category->id = g_strdup("/opds/genres/%D0%94%D0%B5%D0%BB%D0%BE%D0%B2%D0%B0%D1%8F%20%D0%BB%D0%B8%D1%82%D0%B5%D1%80%D0%B0%D1%82%D1%83%D1%80%D0%B0/141");
        category->title = g_strdup("Карьера, кадры");
        g_ptr_array_add(app.subcategories, category);
        category = g_new0(BookRelayCategory, 1);
        category->id = g_strdup("new-unbundled-subcategory");
        category->title = g_strdup("Новая подкатегория без обложки");
        g_ptr_array_add(app.subcategories, category);
    }
    app.view = VIEW_SUBCATEGORIES;
    render_subcategories(&app);
    drain_events();
    button = find_data_button(app.results, "subcategory-row");
    if (!button) g_error("subcategory grid has no cards");
    expect_inside_window(&app, button, "subcategory card");
    snapshot(&app, argv[1], "subcategories.png");

    search_icon_clicked(NULL, &app);
    drain_events();
    expect_visible(app.query, "inline search field");
    expect_inside_window(&app, app.query, "inline search field");
    gtk_widget_grab_focus(app.query);
    drain_events();
    expect_visible(app.keyboard, "search keyboard after focus");
    expect_inside_window(&app, app.keyboard, "search keyboard");
    search_keyboard = g_object_get_data(G_OBJECT(app.keyboard), "bookrelay-keyboard-state");
    letter = g_ptr_array_index(search_keyboard->letter_buttons, 0);
    gtk_button_clicked(GTK_BUTTON(letter));
    drain_events();
    if (g_strcmp0(gtk_entry_get_text(GTK_ENTRY(app.query)), "q") != 0)
        g_error("search keyboard did not insert text");
    snapshot(&app, argv[1], "search-keyboard.png");
    button = find_button(app.keyboard, "Удалить");
    if (!button) g_error("backspace has no readable label");
    gtk_button_clicked(GTK_BUTTON(button));
    if (*gtk_entry_get_text(GTK_ENTRY(app.query))) g_error("backspace did not delete the letter");
    gtk_widget_hide(app.keyboard);

    books = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_book_free);
    book = g_new0(BookRelayBook, 1);
    book->id = g_strdup("layout-test");
    book->title = g_strdup("Очень длинное название книги о путешествии через несколько городов и неожиданных открытиях, которое должно переноситься внутри карточки");
    book->author = g_strdup("Автор с длинным именем и несколькими дополнительными словами");
    book->description = g_strdup("Это длинное описание книги. Оно занимает несколько строк и должно оставаться в пределах экрана даже на более узком разрешении. Ещё один абзац помогает проверить перенос текста в карточке книги.");
    g_ptr_array_add(books, book);
    render_books(&app, books);
    drain_events();
    button = find_data_button(app.results, "book-row");
    if (!button) g_error("book grid has no card");
    expect_inside_window(&app, button, "book card");
    snapshot(&app, argv[1], "book-list.png");
    gtk_button_clicked(GTK_BUTTON(button));
    drain_events();
    details = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-details-page");
    if (!details) g_error("details page not created");
    button = find_button(details->window, "Скачать на Kindle");
    if (!button) g_error("details page has no send button");
    expect_inside_window(&app, button, "send book button");
    snapshot(&app, argv[1], "book-details.png");
    gtk_widget_destroy(details->window);
    drain_events();
    g_ptr_array_free(books, TRUE);

    gtk_widget_destroy(app.window);
    bookrelay_config_free(app.config);
    g_ptr_array_free(app.catalog_categories, TRUE);
    g_ptr_array_free(app.subcategories, TRUE);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
