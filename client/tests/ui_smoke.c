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

static void snapshot(App *app, const gchar *dir, const char *name) {
    GdkPixbuf *pixels;
    gchar *path = g_build_filename(dir, name, NULL);
    GError *error = NULL;
    drain_events();
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
    curl_global_init(CURL_GLOBAL_DEFAULT);
    app.config = g_new0(BookRelayConfig, 1);
    app.config->relay_url = g_strdup("");
    app.config_path = g_build_filename(argv[1], "unused-config.ini", NULL);
    g_mkdir_with_parents(argv[1], 0700);
    build_ui(&app);
    drain_events();
    expect_visible(app.query, "search field");
    expect_inside_window(&app, app.query, "search field");
    expect_visible(app.connection, "connection status");
    gtk_widget_hide(app.keyboard);
    drain_events();
    snapshot(&app, argv[1], "main.png");

    button = find_button(app.window, "Искать");
    if (!button) g_error("search button not found");
    gtk_widget_grab_focus(button);
    drain_events();
    gtk_widget_grab_focus(app.query);
    drain_events();
    expect_visible(app.keyboard, "search keyboard after focus");
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
    button = find_button(app.results, "Подробнее");
    if (!button) g_error("book card has no details button");
    expect_inside_window(&app, button, "book details button");
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

    pair_clicked(NULL, &app);
    drain_events();
    pair = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-pair-page");
    if (!pair) g_error("pairing page not created");
    g_print("pair: page=%d root visible=%d mapped=%d keyboard visible=%d mapped=%d\n",
            gtk_notebook_get_current_page(GTK_NOTEBOOK(app.pages)),
            GTK_WIDGET_VISIBLE(pair->window), GTK_WIDGET_MAPPED(pair->window),
            GTK_WIDGET_VISIBLE(pair->keyboard->root), GTK_WIDGET_MAPPED(pair->keyboard->root));
    snapshot(&app, argv[1], "pairing.png");
    expect_visible(pair->keyboard->root, "pairing keyboard");
    expect_inside_window(&app, GTK_WIDGET(pair->relay), "relay field");
    expect_inside_window(&app, GTK_WIDGET(pair->code), "code field");
    expect_inside_window(&app, pair->keyboard->root, "pairing keyboard");
    virtual_keyboard_show_for(pair->keyboard, pair->code);
    gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(pair->keyboard->letter_buttons, 0)));
    if (g_strcmp0(gtk_entry_get_text(pair->code), "q") != 0)
        g_error("pairing keyboard did not insert into code field");
    gtk_widget_destroy(pair->window);
    drain_events();
    if (app.page_window || gtk_notebook_get_current_page(GTK_NOTEBOOK(app.pages)) != 0)
        g_error("closing pairing did not restore main page");

    settings_clicked(NULL, &app);
    drain_events();
    settings = g_object_get_data(G_OBJECT(app.page_window), "bookrelay-settings-page");
    if (!settings) g_error("settings page not created");
    expect_visible(settings->keyboard->root, "settings keyboard");
    expect_inside_window(&app, GTK_WIDGET(settings->relay), "settings relay field");
    expect_inside_window(&app, settings->keyboard->root, "settings keyboard");
    snapshot(&app, argv[1], "settings.png");
    gtk_widget_destroy(settings->window);
    gtk_widget_destroy(app.window);
    bookrelay_config_free(app.config);
    g_ptr_array_free(app.category_ids, TRUE);
    g_free(app.config_path);
    curl_global_cleanup();
    return 0;
}
