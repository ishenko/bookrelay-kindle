import unittest
import json
from http.client import IncompleteRead
from pathlib import Path
from tempfile import TemporaryDirectory
from time import monotonic
from urllib.error import HTTPError
from unittest.mock import patch

from bookrelay.delivery import MAX_EPUB_BYTES
from bookrelay.source.flibusta import FlibustaSource, SourceUnavailable, parse_opds_feed, parse_search_page
from test_delivery import make_epub


class FlibustaParserTests(unittest.TestCase):
    def test_packaged_catalog_snapshot_serves_navigation_without_source(self):
        snapshot = Path(__file__).parents[2] / 'client/share/subcategories.json'
        source = FlibustaSource(snapshot_path=snapshot)
        with patch.object(source, '_get', side_effect=AssertionError('OPDS navigation requested')):
            categories = source.categories()
            self.assertEqual(len(categories), 24)
            self.assertEqual(sum(len(source.subcategories(item['id'])) for item in categories), 271)
            categories[0]['title'] = 'mutated by caller'
            self.assertNotEqual(source.categories()[0]['title'], 'mutated by caller')
        with self.assertRaises(ValueError):
            source.subcategories('/opds/genres/A/../../other')

    def test_invalid_snapshot_falls_back_to_live_catalog(self):
        with TemporaryDirectory() as directory:
            snapshot = Path(directory) / 'snapshot.json'
            snapshot.write_text(json.dumps([{'category': {'id': '/opds/genres/A', 'title': 'A'},
                                            'subcategories': [{'id': '/elsewhere', 'title': 'bad'}]}]))
            class LiveSource(FlibustaSource):
                def _get(self, path):
                    return (b'<feed xmlns="http://www.w3.org/2005/Atom">'
                            b'<entry><title>Live</title><link rel="subsection" '
                            b'type="application/atom+xml;profile=opds-catalog" '
                            b'href="/opds/genres/Live" /></entry></feed>')
            self.assertEqual(LiveSource(snapshot_path=snapshot).categories()[0]['title'], 'Live')

    def test_transient_upstream_failure_retries_catalog_and_streamed_books(self):
        feed = (b'<feed xmlns="http://www.w3.org/2005/Atom">'
                b'<entry><title>Book</title><link rel="http://opds-spec.org/acquisition/open-access" '
                b'href="/b/123/epub" /></entry></feed>')

        class Response:
            def __init__(self):
                self.reads = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, limit):
                return feed

            def read1(self, limit):
                self.reads += 1
                return feed if self.reads == 1 else b''

        failure = HTTPError('https://flibusta.is/opds/genres', 503, 'temporarily unavailable', {}, None)
        with patch('bookrelay.source.flibusta.urlopen', side_effect=[failure, Response()]) as fetch:
            self.assertEqual(FlibustaSource().categories(), [])
            self.assertEqual(fetch.call_count, 2)
        with patch('bookrelay.source.flibusta.urlopen', side_effect=[failure, Response()]) as fetch:
            books, more = FlibustaSource().catalog_books('/opds/genres/A', '/opds/genres/A/1', 1, 12)
            self.assertEqual([book.id for book in books], ['123'])
            self.assertFalse(more)
            self.assertEqual(fetch.call_count, 2)

    def test_missing_upstream_page_is_not_retried(self):
        failure = HTTPError('https://flibusta.is/opds/genres/A', 404, 'not found', {}, None)
        with patch('bookrelay.source.flibusta.urlopen', side_effect=failure) as fetch:
            with self.assertRaises(SourceUnavailable):
                FlibustaSource().subcategories('/opds/genres/A')
            self.assertEqual(fetch.call_count, 1)

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
            with self.assertRaisesRegex(SourceUnavailable, "exceeds the size limit"):
                FlibustaSource().categories()

    def test_epub_download_uses_epub_limit_instead_of_catalog_limit(self):
        payload = make_epub()

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self, limit):
                self.limit = limit
                return payload

        response = Response()
        with patch('bookrelay.source.flibusta.urlopen', return_value=response):
            self.assertEqual(FlibustaSource().download('123'), payload)
        self.assertEqual(response.limit, MAX_EPUB_BYTES + 1)

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

    def test_opds_standard_thumbnail_is_kept_for_book_cards(self):
        feed = b'''<feed xmlns="http://www.w3.org/2005/Atom">
          <entry><title>Book with cover</title>
            <link rel="http://opds-spec.org/acquisition/open-access" href="/b/451198/epub" />
            <link rel="http://opds-spec.org/image/thumbnail" href="/i/98/451198/cover.jpg" />
          </entry></feed>'''
        _, books, _ = parse_opds_feed(feed, 'https://flibusta.is')
        self.assertEqual(books[0].cover_url, 'https://flibusta.is/i/98/451198/cover.jpg')

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
                    for i in range(26)
                )
                return b'<feed xmlns="http://www.w3.org/2005/Atom">' + entries

        response = SlowResponse()
        with patch('bookrelay.source.flibusta.urlopen', return_value=response):
            books, more = FlibustaSource().catalog_books('/opds/genres/Business', '/opds/genres/Business/141', 1, 12)
        self.assertEqual([book.id for book in books], [str(i) for i in range(12)])
        self.assertTrue(more)
        self.assertEqual(response.reads, 1)

    def test_streamed_books_reuse_bounded_prefix_for_previous_page(self):
        class Response:
            def __init__(self):
                self.reads = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read1(self, limit):
                self.reads += 1
                if self.reads > 1:
                    raise AssertionError('a cached page should not require the end of the feed')
                entries = b''.join(
                    (b'<entry><title>Book %d</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/%d/epub" /></entry>' % (i, i))
                    for i in range(26)
                )
                return b'<feed xmlns="http://www.w3.org/2005/Atom">' + entries

        category, subcategory = '/opds/genres/Business', '/opds/genres/Business/141'
        source = FlibustaSource()
        with patch('bookrelay.source.flibusta.urlopen', side_effect=lambda *args, **kwargs: Response()) as fetch:
            first, more = source.catalog_books(category, subcategory, 1, 12)
            second, more_second = source.catalog_books(category, subcategory, 2, 12)
            again, more_again = source.catalog_books(category, subcategory, 1, 12)
        self.assertEqual([book.id for book in first], [str(i) for i in range(12)])
        self.assertEqual([book.id for book in second], [str(i) for i in range(12, 24)])
        self.assertEqual([book.id for book in again], [book.id for book in first])
        self.assertTrue(more and more_second and more_again)
        self.assertEqual(fetch.call_count, 1)

    def test_sequential_pages_fetch_each_opds_feed_only_once(self):
        class Response:
            def __init__(self, path):
                self.path = path
                self.reads = 0

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read1(self, limit):
                self.reads += 1
                if self.reads > 1:
                    return b''
                prefix = '/opds/genres/Business/141'
                index = int(self.path[len(prefix) + 1:]) if self.path.startswith(prefix + '/') else 0
                next_link = f'<link rel="next" href="/opds/genres/Business/141/{index + 1}" />'
                entries = ''.join(
                    f'<entry><title>Book {i}</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/{i}/epub" /></entry>'
                    for i in range(index * 20, (index + 1) * 20)
                )
                return f'<feed xmlns="http://www.w3.org/2005/Atom">{next_link}{entries}</feed>'.encode()

        def response(request, timeout):
            from urllib.parse import urlsplit
            return Response(urlsplit(request.full_url).path)

        source = FlibustaSource()
        category, subcategory = '/opds/genres/Business', '/opds/genres/Business/141'
        with patch('bookrelay.source.flibusta.urlopen', side_effect=response) as fetch:
            for page in range(1, 11):
                books, has_next = source.catalog_books(category, subcategory, page, 12)
                self.assertEqual([book.id for book in books], [str(i) for i in range((page - 1) * 12, page * 12)])
                self.assertTrue(has_next)
        self.assertEqual(fetch.call_count, 7)

    def test_streamed_book_cache_stays_bounded(self):
        source = FlibustaSource()
        with patch.object(source, '_stream_book_feed', return_value=([], None, True)) as fetch:
            for i in range(20):
                source._book_feed(f'/opds/genres/A/{i}', 13, cache=True)
            source._book_feed('/opds/genres/A/19', 13, cache=True)
        self.assertEqual(len(source._book_cache), 16)
        self.assertEqual(fetch.call_count, 20)

    def test_expired_book_page_survives_transient_source_failure(self):
        source = FlibustaSource()
        category, subcategory = '/opds/genres/A', '/opds/genres/A/1'
        feed = (b'<feed xmlns="http://www.w3.org/2005/Atom">' +
                b''.join(f'<entry><title>Book {i}</title><link rel="http://opds-spec.org/acquisition/open-access" href="/b/{i}/epub" /></entry>'.encode()
                         for i in range(13)) + b'</feed>')
        _, books, _ = parse_opds_feed(feed, source.base_url)
        source._book_cache[subcategory] = (monotonic() - 301, books, None, False)
        with patch.object(source, '_stream_book_feed', side_effect=OSError('upstream timed out')) as fetch:
            first, more = source.catalog_books(category, subcategory, 1, 12)
        self.assertEqual([book.id for book in first], [str(i) for i in range(12)])
        self.assertTrue(more)
        self.assertEqual(fetch.call_count, 1)

        # The same partial prefix cannot claim to answer the next page.
        with patch.object(source, '_stream_book_feed', side_effect=OSError('upstream timed out')) as fetch:
            with self.assertRaises(SourceUnavailable):
                source.catalog_books(category, subcategory, 2, 12)
        self.assertEqual(fetch.call_count, 2)

    def test_expired_book_page_does_not_mask_a_missing_source_page(self):
        source = FlibustaSource()
        path = '/opds/genres/A/1'
        source._book_cache[path] = (monotonic() - 301, [], None, True)
        missing = HTTPError('https://flibusta.is' + path, 404, 'missing', {}, None)
        with patch.object(source, '_stream_book_feed', side_effect=missing) as fetch:
            with self.assertRaises(SourceUnavailable):
                source._book_feed(path, 13, cache=True)
        self.assertEqual(fetch.call_count, 1)

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
        self.assertEqual(source.cover_path('123', 'https://flibusta.is/covers/123.jpg'),
                         '/covers/123.jpg')
        self.assertEqual(source.cover_path('123', 'https://flibusta.is/covers/123.png'),
                         '/covers/123.png')
        self.assertEqual(source.cover_path('451198', 'https://flibusta.is/i/98/451198/cover.jpg'),
                         '/i/98/451198/cover.jpg')
        self.assertEqual(source.cover_path('659948', 'https://flibusta.is/i/48/659948/img_12'),
                         '/i/48/659948/img_12')
        self.assertEqual(source.cover_path('626662', 'https://flibusta.is/i/62/626662/_1551035688_76.jpg'),
                         '/i/62/626662/_1551035688_76.jpg')
        self.assertEqual(source.cover_path('756694', 'https://flibusta.is/i/94/756694/OEBPS/Images/x0000.jpg.jpg'),
                         '/i/94/756694/OEBPS/Images/x0000.jpg.jpg')
        self.assertEqual(source.cover_path('151708', 'https://flibusta.is/i/8/151708/otanix..jpg'),
                         '/i/8/151708/otanix..jpg')
        for url in ('https://elsewhere.test/i/98/451198/cover.jpg',
                    'https://flibusta.is/covers/123.jpg',
                    'https://flibusta.is/covers/451198.jpg?from=other',
                    'https://flibusta.is/i/98/1/cover.jpg',
                    'https://flibusta.is/i/98/451198/cover.jpg?redirect=1',
                    'https://flibusta.is/i/98/451198/../secret',
                    'https://flibusta.is/i/98/451198/OEBPS/../secret',
                    'https://flibusta.is/i/98/451198/OEBPS/%2e%2e/secret',
                    'https://flibusta.is/i/98/451198//secret',
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
