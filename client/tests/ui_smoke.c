/* Run with Xvfb at the Paperwhite resolution. This exercises real GTK widgets,
 * visibility, input and layout; the Python source checks cannot do that. */
#define main bookrelay_application_main
#include "../src/main.c"
#undef main
#include <gdk/gdkkeysyms.h>

static void drain_events(void) {
    while (gtk_events_pending()) gtk_main_iteration();
    gdk_display_sync(gdk_display_get_default());
}

static void tap_search_icon(App *app) {
    GtkWidget *icon = app->search_icon;
    gint x = icon->allocation.x + icon->allocation.width / 2;
    gint y = icon->allocation.y + icon->allocation.height / 2;
    if (!gdk_test_simulate_button(icon->window, x, y, 1, 0, GDK_BUTTON_PRESS))
        g_error("could not press search icon");
    drain_events();
    if (!gdk_test_simulate_button(icon->window, x, y, 1, 0, GDK_BUTTON_RELEASE))
        g_error("could not release search icon");
    drain_events();
}

static void wait_for_pairing(App *app, gboolean expect_error) {
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline) {
        SettingsPage *page;
        drain_events();
        if (!expect_error && app->catalog_ready) return;
        page = app->page_window ? g_object_get_data(G_OBJECT(app->page_window), "bookrelay-settings-page") : NULL;
        if (expect_error && page && page->claimed &&
            GTK_WIDGET_IS_SENSITIVE(g_object_get_data(G_OBJECT(page->window), "bookrelay-settings-connect")) &&
            strstr(gtk_label_get_text(GTK_LABEL(page->feedback)), "почта не сохранена")) return;
        g_usleep(10000);
    }
    g_error("pairing workflow timed out");
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

static GtkWidget *find_cover_image(GtkWidget *root) {
    GList *children, *item;
    GtkWidget *found = NULL;
    if (GTK_IS_IMAGE(root) && gtk_image_get_pixbuf(GTK_IMAGE(root))) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    children = gtk_container_get_children(GTK_CONTAINER(root));
    for (item = children; item && !found; item = item->next)
        found = find_cover_image(GTK_WIDGET(item->data));
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
    app.favorites = bookrelay_favorites_load(app.config_path, app.config->relay_url);
    g_remove(app.favorites->path);
    reload_favorites(&app);
    g_mkdir_with_parents(argv[1], 0700);
    build_ui(&app);
    drain_events();
    settings = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-settings-page");
    if (!settings) g_error("setup screen did not open on first launch");
    if (g_getenv("BOOKRELAY_TEST_PAIR_URL")) {
        BookRelayConfig *saved;
        gtk_entry_set_text(settings->relay, g_getenv("BOOKRELAY_TEST_PAIR_URL"));
        gtk_entry_set_text(settings->email, "reader@kindle.com");
        gtk_entry_set_text(settings->code, "abcdef12");
        g_signal_emit_by_name(settings->code, "activate");
        wait_for_pairing(&app, TRUE);
        if (gtk_entry_get_text_length(settings->code) != 0) g_error("claimed code was not cleared");
        saved = bookrelay_config_load(app.config_path);
        if (g_strcmp0(saved->token, "mock-device-token") != 0)
            g_error("claim token was not saved before email update failed");
        bookrelay_config_free(saved);
        g_signal_emit_by_name(settings->code, "activate");
        wait_for_pairing(&app, FALSE);
        saved = bookrelay_config_load(app.config_path);
        if (g_strcmp0(saved->kindle_email, "reader@kindle.com") != 0)
            g_error("email was not saved after retry without a code");
        bookrelay_config_free(saved);
        tap_search_icon(&app);
        if (gtk_window_get_focus(GTK_WINDOW(app.window)) != app.query)
            g_error("search did not focus after pairing");
        {
            const gchar *character;
            for (character = "test book"; *character; character++) {
                guint key = *character == ' ' ? GDK_space : (guint)*character;
                if (!gdk_test_simulate_key(app.query->window, 12, 12, key, 0, GDK_KEY_PRESS) ||
                    !gdk_test_simulate_key(app.query->window, 12, 12, key, 0, GDK_KEY_RELEASE))
                    g_error("could not type a search query");
            }
        }
        drain_events();
        if (g_strcmp0(gtk_entry_get_text(GTK_ENTRY(app.query)), "test book") != 0)
            g_error("typed search query did not reach the entry");
        tap_search_icon(&app);
        {
            gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
            while (app.active_tasks && g_get_monotonic_time() < deadline) {
                drain_events();
                g_usleep(10000);
            }
        }
        drain_events();
        {
            GtkWidget *result = find_data_button(app.results, "book-row");
            BookRow *row = result ? g_object_get_data(G_OBJECT(result), "book-row") : NULL;
            if (app.active_tasks || app.view != VIEW_SEARCH || !app.has_next || !row ||
                row->book->year != 2022 || g_strcmp0(row->book->title, "A Book") != 0)
                g_error("search icon did not load results from the relay (tasks=%u view=%u ready=%d row=%p status=%s)",
                        app.active_tasks, app.view, app.catalog_ready, row, gtk_label_get_text(GTK_LABEL(app.status)));
        }
        gtk_widget_destroy(app.window);
        bookrelay_config_free(app.config);
        bookrelay_favorites_free(app.favorites);
        g_free(app.config_path);
        curl_global_cleanup();
        return 0;
    }
    if (g_getenv("BOOKRELAY_TEST_NATIVE")) {
        GtkWidget *actions = g_object_get_data(G_OBJECT(settings->window), "bookrelay-page-actions");
        if (!settings->keyboard->native_open)
            g_error("Kindle keyboard open command failed");
        if (GTK_WIDGET_VISIBLE(settings->keyboard->root))
            g_error("native keyboard must not reserve a blank area in the page");
        expect_inside_window(&app, actions, "settings actions with Kindle keyboard");
        snapshot(&app, argv[1], "settings-native.png");
        g_signal_emit_by_name(settings->relay, "activate");
        if (settings->keyboard->native_open) g_error("Enter did not close the Kindle keyboard");
        virtual_keyboard_show_for(settings->keyboard, settings->email);
        if (!settings->keyboard->native_open) g_error("email field did not open Kindle keyboard");
        {
            GtkWidget *viewport = gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(settings->email)));
            if (!gdk_test_simulate_button(viewport->window, viewport->allocation.width / 2,
                                          viewport->allocation.height / 2, 1, 0, GDK_BUTTON_PRESS))
                g_error("could not simulate a tap outside the keyboard");
        }
        drain_events();
        if (settings->keyboard->native_open) g_error("background tap did not close Kindle keyboard");
        virtual_keyboard_show_for(settings->keyboard, settings->code);
        g_signal_emit_by_name(settings->code, "activate");
        if (!*gtk_label_get_text(GTK_LABEL(settings->feedback)))
            g_error("code Enter did not invoke connection validation");
        gtk_widget_destroy(settings->window);
        app.catalog_ready = TRUE;
        search_keyboard = g_object_get_data(G_OBJECT(app.keyboard), "bookrelay-keyboard-state");
        /* Opening from focus-in is too late for the device keyboard: check
         * the toolbar path independently of that fallback handler. */
        g_signal_handlers_block_by_func(app.query, virtual_keyboard_focus_in, search_keyboard);
        /* A real click bubbles to the window's background-tap handler. */
        if (!gdk_test_simulate_button(app.search_icon->window,
                                      app.search_icon->allocation.x + app.search_icon->allocation.width / 2,
                                      app.search_icon->allocation.y + app.search_icon->allocation.height / 2,
                                      1, 0, GDK_BUTTON_PRESS))
            g_error("could not press search icon");
        drain_events();
        search_keyboard = g_object_get_data(G_OBJECT(app.keyboard), "bookrelay-keyboard-state");
        if (search_keyboard->native_open)
            g_error("search keyboard opened before the icon tap finished");
        if (!gdk_test_simulate_button(app.search_icon->window,
                                      app.search_icon->allocation.x + app.search_icon->allocation.width / 2,
                                      app.search_icon->allocation.y + app.search_icon->allocation.height / 2,
                                      1, 0, GDK_BUTTON_RELEASE))
            g_error("could not release search icon");
        drain_events();
        expect_visible(app.query, "native search entry");
        if (gtk_window_get_focus(GTK_WINDOW(app.window)) != app.query)
            g_error("native search entry did not receive keyboard focus");
        search_keyboard = g_object_get_data(G_OBJECT(app.keyboard), "bookrelay-keyboard-state");
        if (!search_keyboard->native_open) g_error("native search did not open the Kindle keyboard");
        {
            GdkEventFocus transient_focus = {0};
            gboolean handled = FALSE;
            transient_focus.type = GDK_FOCUS_CHANGE;
            transient_focus.in = FALSE;
            g_signal_emit_by_name(app.query, "focus-out-event", &transient_focus, &handled);
            if (!search_keyboard->native_open)
                g_error("transient focus loss closed the Kindle search keyboard");
        }
        if (!gdk_test_simulate_key(app.query->window, 12, 12, GDK_a, 0, GDK_KEY_PRESS) ||
            !gdk_test_simulate_key(app.query->window, 12, 12, GDK_a, 0, GDK_KEY_RELEASE))
            g_error("could not simulate native keyboard input");
        drain_events();
        if (g_strcmp0(gtk_entry_get_text(GTK_ENTRY(app.query)), "a") != 0)
            g_error("native keyboard input was not entered into search");
        /* The Kindle input overlay can leave focus on the application window.
         * A key delivered there must still reach the inline search entry. */
        gtk_window_set_focus(GTK_WINDOW(app.window), NULL);
        if (!gdk_test_simulate_key(app.window->window, 12, 12, GDK_b, 0, GDK_KEY_PRESS) ||
            !gdk_test_simulate_key(app.window->window, 12, 12, GDK_b, 0, GDK_KEY_RELEASE))
            g_error("could not simulate key with lost search focus");
        drain_events();
        if (g_strcmp0(gtk_entry_get_text(GTK_ENTRY(app.query)), "ab") != 0)
            g_error("search did not recover from lost keyboard focus: text=%s focus=%p expected=%p", gtk_entry_get_text(GTK_ENTRY(app.query)), gtk_window_get_focus(GTK_WINDOW(app.window)), app.query);
        snapshot(&app, argv[1], "search-native.png");
        gtk_widget_destroy(app.window);
        bookrelay_config_free(app.config);
        bookrelay_favorites_free(app.favorites);
        g_free(app.config_path);
        curl_global_cleanup();
        return 0;
    }
    expect_visible(settings->keyboard->root, "settings keyboard");
    expect_inside_window(&app, GTK_WIDGET(settings->relay), "settings relay field");
    expect_inside_window(&app, GTK_WIDGET(settings->email), "settings email field");
    expect_inside_window(&app, GTK_WIDGET(settings->code), "settings code field");
    if (!GTK_WIDGET_IS_SENSITIVE(GTK_WIDGET(settings->email))) g_error("Kindle email is disabled");
    if (g_strcmp0(normalize_relay_url("example.org"), "https://example.org") != 0)
        g_error("server name was not upgraded to HTTPS");
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

    virtual_keyboard_show_for(settings->keyboard, settings->code);
    if (settings->keyboard->cyrillic) press_key(settings->keyboard->root, "EN");
    gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(settings->keyboard->letter_buttons, 0)));
    if (g_strcmp0(gtk_entry_get_text(settings->code), "q") != 0)
        g_error("keyboard did not insert into code field");
    g_signal_emit_by_name(settings->code, "activate");
    if (!*gtk_label_get_text(GTK_LABEL(settings->feedback)))
        g_error("code Enter did not invoke connection validation");
    snapshot(&app, argv[1], "pairing.png");

    /* Simulate successful setup without starting a network request. */
    app.config->token = g_strdup("smoke-test-token");
    app.catalog_ready = TRUE;
    gtk_widget_destroy(settings->window);
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
    if (gtk_window_get_focus(GTK_WINDOW(app.window)) != app.query)
        g_error("search entry did not receive keyboard focus");
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
    if (g_getenv("BOOKRELAY_TEST_COVER_RELAY")) {
        GtkWidget *image;
        gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
        g_free(app.config->relay_url);
        app.config->relay_url = g_strdup(g_getenv("BOOKRELAY_TEST_COVER_RELAY"));
        reload_favorites(&app);
        g_free(book->id);
        book->id = g_strdup("451198");
        book->cover_url = g_strdup("/v1/books/451198/cover?path=%2Fi%2F98%2F451198%2Fcover.jpg");
        render_books(&app, books);
        while (app.active_tasks && g_get_monotonic_time() < deadline) {
            drain_events();
            g_usleep(10000);
        }
        drain_events();
        if (app.active_tasks) g_error("cover download timed out");
        image = find_cover_image(app.results);
        if (!image || !GTK_WIDGET_VISIBLE(image) || !GTK_WIDGET_MAPPED(image))
            g_error("downloaded cover is not visible");
        if (gdk_pixbuf_get_width(gtk_image_get_pixbuf(GTK_IMAGE(image))) < 100 ||
            gdk_pixbuf_get_height(gtk_image_get_pixbuf(GTK_IMAGE(image))) < 150)
            g_error("downloaded cover was scaled to a tiny image");
        snapshot(&app, argv[1], "book-list-with-cover.png");
        button = find_data_button(app.results, "book-row");
        {
            GPtrArray *many = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_book_free);
            guint i;
            for (i = 0; i < 12; i++) {
                BookRelayBook *item = g_new0(BookRelayBook, 1);
                item->id = g_strdup_printf("%u", 1000 + i);
                item->title = g_strdup("Cover stress test");
                item->cover_url = g_strdup_printf("/v1/books/%s/cover?path=%%2Fi%%2F98%%2F%s%%2Fcover.jpg", item->id, item->id);
                g_ptr_array_add(many, item);
            }
            render_books(&app, many);
            deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
            while (app.active_tasks && g_get_monotonic_time() < deadline) {
                drain_events();
                g_usleep(10000);
            }
            if (app.active_tasks) g_error("bounded cover downloads timed out");
            render_books(&app, books);
            g_ptr_array_free(many, TRUE);
            button = find_data_button(app.results, "book-row");
        }
    }
    gtk_button_clicked(GTK_BUTTON(button));
    drain_events();
    details = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-details-page");
    if (!details) g_error("details page not created");
    button = find_button(details->window, "Скачать на Kindle");
    if (!button) g_error("details page has no send button");
    expect_inside_window(&app, button, "send book button");
    snapshot(&app, argv[1], "book-details.png");
    if (bookrelay_favorites_contains(app.favorites, details->book->id))
        g_error("new book was already a favorite");
    gtk_button_clicked(GTK_BUTTON(details->favorite_button));
    if (!bookrelay_favorites_contains(app.favorites, details->book->id))
        g_error("star did not add book to favorites");
    snapshot(&app, argv[1], "book-details-favorite.png");
    {
        BookRelayFavorites *reloaded = bookrelay_favorites_load(app.config_path, app.config->relay_url);
        BookRelayFavorites unwritable = {0};
        GError *error = NULL;
        unwritable.path = g_strdup("/dev/null/favorites.ini");
        unwritable.books = g_ptr_array_new_with_free_func((GDestroyNotify)bookrelay_book_free);
        if (reloaded->books->len != 1 || !bookrelay_favorites_contains(reloaded, details->book->id))
            g_error("favorite did not survive reload");
        if (bookrelay_favorites_set(&unwritable, details->book, TRUE, &error) || !error || unwritable.books->len)
            g_error("failed write changed favorite state");
        g_clear_error(&error);
        g_ptr_array_free(unwritable.books, TRUE);
        g_free(unwritable.path);
        bookrelay_favorites_free(reloaded);
    }
    gtk_widget_destroy(details->window);
    drain_events();
    show_delivery_page(&app);
    drain_events();
    if (!app.delivery_page || !app.delivery_label ||
        g_strcmp0(gtk_label_get_text(GTK_LABEL(app.delivery_label)), "Книга скачивается…") != 0)
        g_error("download progress page did not open");
    expect_inside_window(&app, app.delivery_label, "download progress");
    snapshot(&app, argv[1], "download-progress.png");
    gtk_widget_destroy(app.delivery_page);
    if (app.delivery_page || app.delivery_label) g_error("download progress page pointers were not cleared");
    drain_events();
    {
        GdkEventButton event = {0};
        event.button = 1;
        favorites_icon_pressed(NULL, &event, &app);
    }
    drain_events();
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(app.section_title)), "Избранное") != 0)
        g_error("favorites view title is missing");
    button = find_data_button(app.results, "book-row");
    if (!button) g_error("favorite book card is missing");
    expect_inside_window(&app, button, "favorite card");
    snapshot(&app, argv[1], "favorites.png");
    {
        guint i;
        for (i = 0; i < 12; i++) {
            BookRelayBook extra = {0};
            GError *error = NULL;
            extra.id = g_strdup_printf("extra-%u", i);
            extra.title = g_strdup_printf("Избранная книга %u", i);
            extra.author = g_strdup("Автор");
            if (!bookrelay_favorites_set(app.favorites, &extra, TRUE, &error))
                g_error("add test favorite: %s", error->message);
            g_free(extra.id); g_free(extra.title); g_free(extra.author);
        }
        navigate_view(&app, VIEW_FAVORITES, 2);
        drain_events();
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(app.page_label)), "Страница 2 из 2") != 0 ||
            !find_data_button(app.results, "book-row"))
            g_error("favorite pagination failed");
        snapshot(&app, argv[1], "favorites-page-2.png");
        button = find_data_button(app.results, "book-row");
        gtk_button_clicked(GTK_BUTTON(button));
        details = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-details-page");
        gtk_button_clicked(GTK_BUTTON(details->favorite_button));
        if (app.page != 1 || g_strcmp0(gtk_label_get_text(GTK_LABEL(app.page_label)), "Страница 1 из 1") != 0)
            g_error("removing the last card on page two did not return to page one");
        gtk_widget_destroy(details->window);
        drain_events();
        for (i = 0; i < 11; i++) {
            BookRelayBook extra = {0};
            gchar *id = g_strdup_printf("extra-%u", i);
            extra.id = id;
            if (!bookrelay_favorites_set(app.favorites, &extra, FALSE, NULL))
                g_error("remove test favorite failed");
            g_free(id);
        }
    }
    navigate_view(&app, VIEW_FAVORITES, 1);
    button = find_data_button(app.results, "book-row");
    gtk_button_clicked(GTK_BUTTON(button));
    details = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-details-page");
    gtk_button_clicked(GTK_BUTTON(details->favorite_button));
    if (app.favorites->books->len || find_data_button(app.results, "book-row") || app.page != 1)
        g_error("removing the last favorite did not refresh the empty page");
    gtk_widget_destroy(details->window);
    drain_events();
    snapshot(&app, argv[1], "favorites-empty.png");
    g_ptr_array_free(books, TRUE);

    gtk_widget_destroy(app.window);
    bookrelay_config_free(app.config);
    bookrelay_favorites_free(app.favorites);
    g_ptr_array_free(app.catalog_categories, TRUE);
    g_ptr_array_free(app.subcategories, TRUE);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
