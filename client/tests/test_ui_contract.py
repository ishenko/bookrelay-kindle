import re
import unittest
from pathlib import Path


class KindleUiContractTests(unittest.TestCase):
    def test_pairing_status_displays_the_one_time_code(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertRegex(source, r'g_strdup_printf\("Код pairing: %s')

    def test_search_ui_has_page_navigation(self):
        source = Path(__file__).parents[1].joinpath("src", "main.c").read_text()
        self.assertIn("guint page;", source)
        self.assertIn('gtk_button_new_with_label("Назад")', source)
        self.assertIn('gtk_button_new_with_label("Дальше")', source)


if __name__ == "__main__":
    unittest.main()
