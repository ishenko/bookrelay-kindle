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

    def test_native_ui_has_confirmed_exit_action(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn('gtk_button_new_with_label("Выйти")', source)
        self.assertIn("exit_clicked", source)
        self.assertIn("gtk_main_quit", source)

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
