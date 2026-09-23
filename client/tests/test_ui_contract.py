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
        self.assertIn("Одноразовый код", source)
        self.assertNotIn("poll_pairing", source)
        self.assertNotIn("TASK_PAIR_STATUS", source)
        self.assertNotIn("Откройте relay /pair", source)

    def test_search_ui_has_page_navigation(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("guint page;", source)
        self.assertIn('gtk_button_new_with_label("Назад")', source)
        self.assertIn('gtk_button_new_with_label("Дальше")', source)

    def test_native_search_passes_category_and_handles_empty_state(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        api = Path(__file__).parents[1].joinpath("src", "api.c").read_text()
        self.assertIn("selected_category", source)
        self.assertIn("category=", api)
        self.assertIn("Ничего не найдено", source)
        self.assertIn("Введите запрос", source)

    def test_native_inputs_have_explicit_focus_and_enter_search(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("gtk_widget_grab_focus", source)
        self.assertIn("gtk_window_set_focus", source)
        self.assertIn("search_entry_activate", source)
        self.assertIn("gtk_entry_set_activates_default", source)

    def test_pairing_dialog_is_large_and_validates_relay_url(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("gtk_window_set_default_size", source)
        self.assertIn("https://", source)
        self.assertIn("Relay URL должен начинаться", source)
        self.assertIn("gtk_entry_set_width_chars", source)

    def test_native_cards_have_visible_cover_area_and_status(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("gtk_frame_new", source)
        self.assertIn("gtk_widget_set_size_request", source)
        self.assertIn("Обложка недоступна", source)
        self.assertIn("gtk_widget_set_no_show_all(placeholder, TRUE)", source)
        self.assertIn("Найдено книг", source)

    def test_results_box_is_wrapped_for_kindlehf_scrolled_window(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn(
            "gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), app->results)",
            source,
        )
        self.assertNotIn("gtk_container_add(GTK_CONTAINER(scroll), app->results)", source)

    def test_native_ui_has_confirmed_exit_action(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn('gtk_button_new_with_label("Выйти")', source)
        self.assertIn("exit_clicked", source)
        self.assertIn("gtk_main_quit", source)

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


if __name__ == "__main__":
    unittest.main()
