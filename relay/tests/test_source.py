import unittest

from bookrelay.source.flibusta import parse_search_page


class FlibustaParserTests(unittest.TestCase):
    def test_parses_book_cards_and_ignores_non_book_links(self):
        html = """
        <html><body>
          <a href="/booksearch?ask=cat">search</a>
          <article class="book-card">
            <a href="/b/123">The Left Hand of Darkness</a>
            <span class="author">Ursula K. Le Guin</span>
            <img src="/covers/123.jpg" alt="cover">
          </article>
          <article class="book-card">
            <a href="/b/456">Another Book</a>
          </article>
        </body></html>
        """
        books = parse_search_page(html, "https://flibusta.is")
        self.assertEqual([book.id for book in books], ["123", "456"])
        self.assertEqual(books[0].title, "The Left Hand of Darkness")
        self.assertEqual(books[0].author, "Ursula K. Le Guin")
        self.assertEqual(books[0].cover_url, "https://flibusta.is/covers/123.jpg")


if __name__ == "__main__":
    unittest.main()
