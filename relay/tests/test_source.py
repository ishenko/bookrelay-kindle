import unittest
from http.client import IncompleteRead
from unittest.mock import patch

from bookrelay.source.flibusta import FlibustaSource, SourceUnavailable, parse_opds_feed, parse_search_page


class FlibustaParserTests(unittest.TestCase):
    def test_truncated_catalog_connection_is_source_outage(self):
        class TruncatedResponse:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, limit):
                raise IncompleteRead(b"<feed")

            def read1(self, limit):
                raise IncompleteRead(b"<feed")

        with patch("bookrelay.source.flibusta.urlopen", return_value=TruncatedResponse()):
            source = FlibustaSource()
            with self.assertRaisesRegex(SourceUnavailable, "did not respond"):
                source.categories()
            with self.assertRaisesRegex(SourceUnavailable, "did not respond"):
                source.catalog_books("/opds/genres/A", "/opds/genres/A/1")

    def test_oversized_source_response_is_reported_as_unavailable(self):
        class OversizedResponse:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, limit):
                return b"x" * limit

        with patch("bookrelay.source.flibusta.urlopen", return_value=OversizedResponse()):
            with self.assertRaisesRegex(SourceUnavailable, "exceeds 25 MiB"):
                FlibustaSource().categories()

    def test_catalog_uses_stale_cached_feed_during_source_outage(self):
        class StubSource(FlibustaSource):
            def __init__(self):
                super().__init__()
                self.offline = False

            def _get(self, path):
                if self.offline:
                    raise SourceUnavailable('offline')
                return (b'<feed xmlns="http://www.w3.org/2005/Atom">'
                        b'<entry><title>Books</title><link rel="subsection" '
                        b'type="application/atom+xml;profile=opds-catalog" href="/opds/genres/1" />'
                        b'</entry></feed>')

        source = StubSource()
        expected = source.categories()
        source.offline = True
        cached_time, cached_feed = source._feed_cache['/opds/genres']
        source._feed_cache['/opds/genres'] = (cached_time - 301, cached_feed)
        self.assertEqual(source.categories(), expected)
        source.offline = False
        source._get = lambda path: b'<html>temporarily unavailable'
        self.assertEqual(source.categories(), expected)
        self.assertEqual(source._feed_cache['/opds/genres'][1], cached_feed)
        source._get = lambda path: b'<html><body>temporarily unavailable</body></html>'
        self.assertEqual(source.categories(), expected)
        self.assertEqual(source._feed_cache['/opds/genres'][1], cached_feed)

    def test_invalid_source_feed_is_service_unavailable(self):
        class StubSource(FlibustaSource):
            def _get(self, path):
                return b'<html>temporarily unavailable'

        source = StubSource()
        with self.assertRaises(SourceUnavailable):
            source.categories()
        with self.assertRaises(SourceUnavailable):
            source.catalog_books('/opds/genres/1', '/opds/genres/1/2')
        source._get = lambda path: b'<html><body>temporarily unavailable</body></html>'
        with self.assertRaises(SourceUnavailable):
            source.categories()
        with self.assertRaises(SourceUnavailable):
            source.catalog_books('/opds/genres/1', '/opds/genres/1/2')

    def test_search_requests_book_results_not_search_navigation(self):
        class StubSource(FlibustaSource):
            def _get(self, path):
                self.path = path
                return (b'<feed xmlns="http://www.w3.org/2005/Atom">'
                        b'<entry><title>Book</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/123/epub" /></entry>'
                        b'</feed>')
        source = StubSource()
        books, more = source.search_page('War & Peace', 1, 12)
        self.assertEqual(source.path, '/opds/search?searchType=books&searchTerm=War+%26+Peace')
        self.assertEqual([book.id for book in books], ['123'])
        self.assertFalse(more)

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
                                          '/opds/genres/7?page=2',
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

    def test_streaming_stops_when_first_page_is_ready(self):
        class SlowResponse:
            def __init__(self):
                self.reads = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read1(self, limit):
                self.reads += 1
                if self.reads > 1:
                    raise AssertionError('read the rest of a huge OPDS feed')
                entries = b''.join(
                    ('<entry><title>Book %d</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/%d/epub" /></entry>' % (i, i)).encode()
                    for i in range(13)
                )
                return b'<feed xmlns="http://www.w3.org/2005/Atom">' + entries

        response = SlowResponse()
        with patch('bookrelay.source.flibusta.urlopen', return_value=response):
            books, more = FlibustaSource().catalog_books('/opds/genres/Business', '/opds/genres/Business/141', 1, 12)
        self.assertEqual([book.id for book in books], [str(i) for i in range(12)])
        self.assertTrue(more)
        self.assertEqual(response.reads, 1)

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
        self.assertEqual(source.cover_path('659948', 'https://flibusta.is/i/48/659948/img_12'),
                         '/i/48/659948/img_12')
        self.assertEqual(source.cover_path('626662', 'https://flibusta.is/i/62/626662/_1551035688_76.jpg'),
                         '/i/62/626662/_1551035688_76.jpg')
        for url in ('https://elsewhere.test/i/98/451198/cover.jpg',
                    'https://flibusta.is/i/98/1/cover.jpg',
                    'https://flibusta.is/i/98/451198/cover.jpg?redirect=1',
                    'https://flibusta.is/i/98/451198/../secret',
                    'https://flibusta.is/i/98/451198/%2e%2e'):
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
