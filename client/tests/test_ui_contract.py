import hashlib
import json
import unittest
from pathlib import Path


class KindleUiContractTests(unittest.TestCase):
    def test_pairing_claim_uses_relay_url_and_code(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        api = Path(__file__).parents[1].joinpath("src", "api.c").read_text()
        header = Path(__file__).parents[1].joinpath("src", "api.h").read_text()
        self.assertIn("TASK_PAIR_CLAIM", source)
        self.assertIn("bookrelay_api_pair_claim", api)
        self.assertIn("bookrelay_api_pair_claim", header)
        self.assertIn('"/v1/pair/claim"', api)
        self.assertIn("одноразовый код", source.lower())
        self.assertNotIn("poll_pairing", source)
        self.assertNotIn("TASK_PAIR_STATUS", source)
        self.assertNotIn("Откройте relay /pair", source)

    def test_search_ui_has_page_navigation(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("guint page;", source)
        for control, icon in (("first_page", "first"), ("previous_page", "previous"),
                              ("next_page", "next"), ("last_page", "last")):
            self.assertIn(f'app->{control} = gtk_button_new()', source)
            self.assertIn(f'ink_icon_image("{icon}.png")', source)
        self.assertIn('g_strdup_printf("Страница %u из %u"', source)
        self.assertIn('gtk_box_pack_end(GTK_BOX(root), navigation', source)

    def test_native_search_passes_category_and_handles_empty_state(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        api = Path(__file__).parents[1].joinpath("src", "api.c").read_text()
        self.assertIn("bookrelay_api_search(task->base_url", source)
        self.assertIn('"/v1/catalog/books"', api)
        self.assertIn("Ничего не найдено", source)
        self.assertIn("Введите запрос", source)

    def test_native_inputs_have_explicit_focus_and_enter_search(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("gtk_widget_grab_focus", source)
        self.assertIn("gtk_window_set_focus", source)
        self.assertIn("search_entry_activate", source)
        self.assertIn("gtk_entry_set_activates_default", source)
        self.assertIn("virtual_keyboard_bind(page->keyboard, page->code)", source)
        self.assertIn("virtual_keyboard_new(root, GTK_ENTRY(app->query))", source)
        self.assertIn("gtk_widget_show(keyboard->root)", source)
        self.assertIn("gtk_widget_show(app->search_row)", source)

    def test_pairing_dialog_is_large_and_validates_relay_url(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("gtk_window_set_default_size", source)
        self.assertIn("https://", source)
        self.assertIn("normalize_relay_url", source)
        self.assertIn("display_relay_url", source)
        self.assertIn("gtk_entry_set_width_chars", source)

    def test_native_cards_have_visible_cover_area_and_year(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("gtk_frame_new", source)
        self.assertIn("gtk_widget_set_size_request", source)
        self.assertIn('book->year ? g_strdup_printf("%d", book->year)', source)
        self.assertIn("gtk_widget_set_no_show_all(placeholder, TRUE)", source)
        self.assertIn("gtk_label_set_width_chars(GTK_LABEL(label)", source)

    def test_catalog_grid_and_no_main_scroll(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("static void render_categories(App *app)", source)
        self.assertIn("static void render_subcategories(App *app)", source)
        self.assertIn("static void render_books(App *app, GPtrArray *books)", source)
        self.assertIn("gtk_box_pack_start(GTK_BOX(root), app->results", source)
        self.assertNotIn("gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), app->results)", source)

    def test_native_ui_has_confirmed_exit_action(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn('"Выйти", GTK_RESPONSE_ACCEPT', source)
        self.assertIn("exit_clicked", source)
        self.assertIn("gtk_main_quit", source)

    def test_category_covers_and_header_icons_are_packaged(self):
        root = Path(__file__).parents[2]
        source = root.joinpath("client", "src", "main.c").read_text()
        packager = root.joinpath("scripts", "package-kpm.py").read_text()
        covers = list(root.joinpath("client", "share", "covers").glob("*.jpg"))
        snapshot = json.loads(root.joinpath("client", "share", "subcategories.json").read_text())
        subcategories = [item for group in snapshot for item in group["subcategories"]]
        self.assertEqual(len(snapshot), 24)
        self.assertEqual(len(subcategories), 271)
        self.assertEqual(len(covers), 24 + len(subcategories))
        for item in subcategories:
            filename = hashlib.sha256(item["id"].encode()).hexdigest() + ".jpg"
            self.assertTrue(root.joinpath("client", "share", "covers", filename).is_file(), item["title"])
        self.assertIn('header_icon("help.png"', source)
        self.assertIn('header_icon("settings.png"', source)
        self.assertIn('header_icon("home.png"', source)
        self.assertIn('share/icons/{icon.name}', packager)
        for icon in ("home", "search", "settings", "help", "close", "first", "previous", "next", "last"):
            self.assertTrue(root.joinpath("client", "share", "icons", f"{icon}.png").is_file())

    def test_native_windows_use_kindle_window_manager_titles(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn('KINDLE_APP_WINDOW_TITLE "L:A_N:application_PC:T_ID:bookrelay.kindle"', source)
        self.assertIn('KINDLE_DIALOG_WINDOW_TITLE "L:D_N:dialog_M:dismissable_ID:bookrelay.kindle.dialog"', source)
        self.assertIn("set_kindle_dialog_role(dialog)", source)

    def test_settings_do_not_expose_server_generated_device_id(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        config = Path(__file__).parents[1].joinpath("src", "config.c").read_text()
        self.assertNotIn('gtk_label_new("Device ID")', source)
        self.assertNotIn("config->device_id", config)

    def test_browser_preview_has_exit_action(self):
        root = Path(__file__).parents[2]
        index = root.joinpath("preview", "index.html").read_text()
        app = root.joinpath("preview", "app.js").read_text()
        self.assertIn('id="exit-button"', index)
        self.assertIn("openExitConfirmation", app)
        self.assertIn("Приложение закрыто", app)

    def test_native_client_bounds_network_and_cache_work(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        api = Path(__file__).parents[1].joinpath("src", "api.c").read_text()
        self.assertIn("MAX_COVER_CACHE_BYTES", source)
        self.assertIn("g_compute_checksum_for_string", source)
        self.assertIn("MAX_API_RESPONSE_BYTES", api)
        self.assertIn("MAX_COVER_RESPONSE_BYTES", api)
        self.assertIn("response->exceeded", api)

    def test_native_network_work_runs_outside_gtk_main_loop(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("g_thread_new", source)
        self.assertIn("delete_event", source)

    def test_native_page_windows_and_results_keep_visible_layout(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("new_kindle_page", source)
        self.assertNotIn("gtk_widget_set_size_request(app->results, 0, 0)", source)

    def test_native_navigation_stays_inside_one_top_level_window(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("app->pages = gtk_notebook_new()", source)
        self.assertIn("gtk_notebook_set_show_tabs(GTK_NOTEBOOK(app->pages), FALSE)", source)
        self.assertIn("gtk_notebook_append_page(GTK_NOTEBOOK(app->pages), root, NULL)", source)
        self.assertNotIn("gtk_window_present(GTK_WINDOW(app->page_window))", source)
        self.assertNotIn("gtk_window_set_title(GTK_WINDOW(window), KINDLE_APP_WINDOW_TITLE)", source)

    def test_native_api_joins_relay_urls_and_reports_error_details(self):
        api = Path(__file__).parents[1].joinpath("src", "api.c").read_text()
        self.assertIn('gchar *endpoint = join_url(base_url, "/v1/search")', api)
        self.assertIn('relay returned HTTP %ld: %s', api)
        self.assertIn('gchar *path = g_strdup_printf("/v1/deliveries/%s", job_id)', api)


if __name__ == "__main__":
    unittest.main()
