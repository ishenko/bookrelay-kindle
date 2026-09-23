import unittest

from bookrelay.source.flibusta import FlibustaSource, parse_opds_feed, parse_search_page


class FlibustaParserTests(unittest.TestCase):
    def test_opds_navigation_and_book_metadata(self):
        feed = b'''<feed xmlns="http://www.w3.org/2005/Atom" xmlns:dcterms="http://purl.org/dc/terms/">
          <link rel="next" href="/opds/genres/7?page=2" />
          <entry><title>Fantasy</title><link rel="subsection" type="application/atom+xml;profile=opds-catalog" href="/opds/genres/7" /></entry>
          <entry><title>A Book</title><author><name>A Writer</name></author><dcterms:issued>2022-01-01</dcterms:issued>
            <link rel="http://opds-spec.org/acquisition/open-access" href="/b/123/epub" />
            <link rel="http://opds-spec.org/thumbnail" href="/covers/123.jpg" />
          </entry></feed>'''
        sections, books, has_next = parse_opds_feed(feed, 'https://flibusta.is')
        self.assertEqual(sections, [{'id': '/opds/genres/7', 'title': 'Fantasy'}])
        self.assertEqual([(book.id, book.title, book.author, book.year, book.cover_url) for book in books],
                         [('123', 'A Book', 'A Writer', 2022, 'https://flibusta.is/covers/123.jpg')])
        self.assertTrue(has_next)

    def test_opds_paging_follows_next_and_stops_at_last_page(self):
        class StubSource(FlibustaSource):
            def __init__(self):
                super().__init__()
                self.requests = []

            def _get(self, path):
                self.requests.append(path)
                page = '1' if '?' not in path else '2'
                next_link = '<link rel="next" href="/opds/genres/7?page=2" />' if page == '1' else ''
                return (f'<feed xmlns="http://www.w3.org/2005/Atom">{next_link}'
                        f'<entry><title>Book {page}</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/{page}/epub" /></entry>'
                        '</feed>').encode()

        source = StubSource()
        first, more = source._paged_books('/opds/genres/7', 1, 1)
        second, last = source._paged_books('/opds/genres/7', 2, 1)
        self.assertEqual([book.id for book in first], ['1'])
        self.assertEqual([book.id for book in second], ['2'])
        self.assertTrue(more)
        self.assertFalse(last)
        self.assertEqual(source.requests, ['/opds/genres/7',
                                          '/opds/genres/7', '/opds/genres/7?page=2'])

    def test_catalog_first_page_does_not_refetch_navigation(self):
        class StubSource(FlibustaSource):
            def __init__(self):
                super().__init__()
                self.requests = []

            def _get(self, path):
                self.requests.append(path)
                return (b'<feed xmlns="http://www.w3.org/2005/Atom">'
                        b'<entry><title>Book</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/123/epub" /></entry>'
                        b'</feed>')

        source = StubSource()
        books, more = source.catalog_books('/opds/genres/Business', '/opds/genres/Business/144', 1, 12)
        self.assertEqual([book.id for book in books], ['123'])
        self.assertFalse(more)
        self.assertEqual(source.requests, ['/opds/genres/Business/144'])
        with self.assertRaises(ValueError):
            source.catalog_books('/opds/genres/Business', 'https://elsewhere.test/secret')

    def test_catalog_next_page_reuses_previous_opds_feed(self):
        class StubSource(FlibustaSource):
            def __init__(self):
                super().__init__()
                self.requests = []

            def _get(self, path):
                self.requests.append(path)
                next_link = '<link rel="next" href="/opds/genres/Business/144/1" />' if path.endswith('/144') else ''
                book_id = '1' if path.endswith('/144') else '2'
                return (f'<feed xmlns="http://www.w3.org/2005/Atom">{next_link}'
                        f'<entry><title>Book</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/{book_id}/epub" /></entry>'
                        '</feed>').encode()

        source = StubSource()
        category, subcategory = '/opds/genres/Business', '/opds/genres/Business/144'
        source.catalog_books(category, subcategory, 1, 1)
        second, more = source.catalog_books(category, subcategory, 2, 1)
        self.assertEqual([book.id for book in second], ['2'])
        self.assertFalse(more)
        self.assertEqual(source.requests, [subcategory, subcategory + '/1'])

    def test_cover_source_is_restricted_to_matching_book_and_host(self):
        source = FlibustaSource()
        self.assertEqual(source.cover_path('451198', 'https://flibusta.is/i/98/451198/cover.jpg'),
                         '/i/98/451198/cover.jpg')
        for url in ('https://elsewhere.test/i/98/451198/cover.jpg',
                    'https://flibusta.is/i/98/1/cover.jpg',
                    'https://flibusta.is/i/98/451198/cover.jpg?redirect=1'):
            with self.assertRaises(ValueError):
                source.cover_path('451198', url)

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
