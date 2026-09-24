import html
import http.client
import json
import re
from collections import OrderedDict
from pathlib import Path
from urllib.error import HTTPError, URLError
from threading import Lock
from time import monotonic
from xml.etree import ElementTree as ET
from urllib.parse import quote_plus, urljoin, urlsplit
from urllib.request import Request, urlopen

from ..delivery import MAX_EPUB_BYTES, validate_epub
from ..models import Book


TAG_RE = re.compile(r"<[^>]+>")
ATOM = "{http://www.w3.org/2005/Atom}"
DC = "{http://purl.org/dc/terms/}"


class SourceUnavailable(Exception):
    pass


def retryable_source_error(exc: Exception) -> bool:
    # Missing books and covers will not appear on a second request. The OPDS
    # host does intermittently fail with 5xx or a dropped connection.
    return not isinstance(exc, HTTPError) or exc.code == 429 or exc.code >= 500


def parse_feed_root(payload: bytes) -> ET.Element:
    try:
        root = ET.fromstring(payload)
    except ET.ParseError as exc:
        raise SourceUnavailable("Flibusta returned an invalid catalog") from exc
    if root.tag != f"{ATOM}feed":
        raise SourceUnavailable("Flibusta returned an invalid catalog")
    return root


def parse_opds_feed(payload: bytes, base_url: str) -> tuple[list[dict[str, str]], list[Book], bool]:
    root = parse_feed_root(payload)
    sections, books = [], []
    has_next = any(link.get("rel") == "next" for link in root.findall(f"{ATOM}link"))
    for entry in root.findall(f"{ATOM}entry"):
        title = (entry.findtext(f"{ATOM}title") or "").strip()
        links = entry.findall(f"{ATOM}link")
        section = next((link for link in links if "opds-catalog" in link.get("type", "") and link.get("rel", "") in ("", "subsection")), None)
        if section is not None:
            sections.append({"id": section.get("href", ""), "title": title})
            continue
        book = parse_book_entry(entry, base_url)
        if book:
            books.append(book)
    return sections, books, has_next


def parse_book_entry(entry: ET.Element, base_url: str) -> Book | None:
    links = entry.findall(f"{ATOM}link")
    acquisition = next((link for link in links if "/b/" in link.get("href", "") and "acquisition" in link.get("rel", "")), None)
    if acquisition is None:
        return None
    match = re.search(r"/b/([0-9]+)", acquisition.get("href", ""))
    if not match:
        return None
    cover = next((link.get("href", "") for link in links if link.get("rel") in ("http://opds-spec.org/image/thumbnail", "http://opds-spec.org/image", "http://opds-spec.org/thumbnail")), "")
    issued = entry.findtext(f"{DC}issued") or ""
    content = entry.findtext(f"{ATOM}content") or ""
    try:
        cover_url = urljoin(base_url, cover) if cover else ""
    except ValueError:
        # One malformed image link must not fail the entire OPDS book page.
        cover_url = ""
    return Book(id=match.group(1), title=(entry.findtext(f"{ATOM}title") or "").strip(),
                author=", ".join(author.findtext(f"{ATOM}name") or "" for author in entry.findall(f"{ATOM}author")),
                cover_url=cover_url,
                description=clean_text(content)[:2000],
                year=int(issued[:4]) if re.fullmatch(r"\d{4}", issued[:4]) else None)


def clean_text(value: str) -> str:
    return re.sub(r"\s+", " ", html.unescape(TAG_RE.sub(" ", value))).strip()


def parse_search_page(page: str, base_url: str) -> list[Book]:
    cards = re.findall(r"<article\b[^>]*class=[\"'][^\"']*book-card[^\"']*[\"'][^>]*>(.*?)</article>", page, re.I | re.S)
    if not cards:
        cards = re.findall(r"(<a\b[^>]*href=[\"']/b/[^\"']+[\"'][^>]*>.*?</a>)", page, re.I | re.S)
    books = []
    seen = set()
    for card in cards:
        link = re.search(r"href=[\"'](?:[^\"']*?)(?:/b/)([A-Za-z0-9_-]+)[\"']", card, re.I)
        if not link or link.group(1) in seen:
            continue
        book_id = link.group(1)
        title_match = re.search(r"<a\b[^>]*href=[\"'][^\"']*/b/[^\"']+[\"'][^>]*>(.*?)</a>", card, re.I | re.S)
        author_match = re.search(r"<[^>]*class=[\"'][^\"']*author[^\"']*[\"'][^>]*>(.*?)</", card, re.I | re.S)
        cover_match = re.search(r"<img\b[^>]*src=[\"']([^\"']+)[\"']", card, re.I)
        books.append(
            Book(
                id=book_id,
                title=clean_text(title_match.group(1)) if title_match else book_id,
                author=clean_text(author_match.group(1)) if author_match else "",
                cover_url=urljoin(base_url, html.unescape(cover_match.group(1))) if cover_match else "",
            )
        )
        seen.add(book_id)
    return books


class FlibustaSource:
    def __init__(self, base_url: str = "https://flibusta.is", timeout: int = 12,
                 snapshot_path: Path | None = None):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self._snapshot_categories: list[dict[str, str]] | None = None
        self._snapshot_subcategories: dict[str, list[dict[str, str]]] = {}
        if snapshot_path is not None:
            try:
                groups = json.loads(snapshot_path.read_text(encoding="utf-8"))
                categories = []
                subcategories = {}
                for group in groups:
                    category = group["category"]
                    category_id = category["id"]
                    if not re.fullmatch(r"/opds/genres/[^/?#]+", category_id):
                        raise ValueError("invalid category in catalog snapshot")
                    items = group["subcategories"]
                    if not all(re.fullmatch(re.escape(category_id) + r"/[^/?#]+", item["id"])
                               for item in items):
                        raise ValueError("invalid subcategory in catalog snapshot")
                    categories.append({"id": category_id, "title": category["title"]})
                    subcategories[category_id] = [{"id": item["id"], "title": item["title"]} for item in items]
                if not categories or len(subcategories) != len(categories):
                    raise ValueError("empty or duplicate catalog snapshot")
                self._snapshot_categories = categories
                self._snapshot_subcategories = subcategories
            except (OSError, ValueError, KeyError, TypeError):
                # An absent or damaged snapshot must never break the live OPDS path.
                pass
        self._feed_cache: OrderedDict[str, tuple[float, bytes]] = OrderedDict()
        self._feed_cache_bytes = 0
        self._feed_cache_lock = Lock()
        # Streaming stops as soon as a page is ready. Retain a small prefix so
        # returning to a page does not download the same large OPDS feed again.
        self._book_cache: OrderedDict[str, tuple[float, list[Book], str | None, bool]] = OrderedDict()
        self._book_cache_lock = Lock()

    def _get(self, path: str, max_bytes: int = 25 * 1024 * 1024) -> bytes:
        request = Request(urljoin(self.base_url + "/", path.lstrip("/")), headers={"User-Agent": "BookRelay/0.1"})
        for attempt in range(2):
            try:
                with urlopen(request, timeout=self.timeout) as response:
                    payload = response.read(max_bytes + 1)
                    if len(payload) > max_bytes:
                        raise SourceUnavailable("Flibusta response exceeds the size limit")
                    return payload
            except (HTTPError, URLError, OSError, TimeoutError, http.client.HTTPException) as exc:
                if attempt or not retryable_source_error(exc):
                    raise SourceUnavailable("Flibusta did not respond; please retry") from exc

    def search(self, query: str, page: int = 1, category: str | None = None) -> list[Book]:
        return self.search_page(query, page)[0]

    def search_page(self, query: str, page: int = 1, size: int = 6) -> tuple[list[Book], bool]:
        suffix = f"/opds/search?searchType=books&searchTerm={quote_plus(query.strip())}"
        return self._paged_books(suffix, page, size, cache=True)

    def _catalog_feed(self, path: str) -> bytes:
        now = monotonic()
        with self._feed_cache_lock:
            cached = self._feed_cache.get(path)
            if cached and now - cached[0] < 300:
                self._feed_cache.move_to_end(path)
                return cached[1]
        try:
            payload = self._get(path)
        except SourceUnavailable:
            if cached:
                return cached[1]
            raise
        try:
            parse_feed_root(payload)
        except SourceUnavailable:
            if cached:
                return cached[1]
            raise
        if len(payload) <= 512 * 1024:
            with self._feed_cache_lock:
                old = self._feed_cache.pop(path, None)
                if old:
                    self._feed_cache_bytes -= len(old[1])
                self._feed_cache[path] = (monotonic(), payload)
                self._feed_cache_bytes += len(payload)
                while len(self._feed_cache) > 64 or self._feed_cache_bytes > 8 * 1024 * 1024:
                    _, (_, removed) = self._feed_cache.popitem(last=False)
                    self._feed_cache_bytes -= len(removed)
        return payload

    def _book_feed(self, path: str, needed: int, cache: bool) -> tuple[list[Book], str | None]:
        # Test and alternate sources supply complete feeds through _get().
        if type(self)._get is not FlibustaSource._get:
            payload = self._catalog_feed(path) if cache else self._get(path)
            root = parse_feed_root(payload)
            next_link = next((link.get("href") for link in root.findall(f"{ATOM}link") if link.get("rel") == "next"), None)
            return parse_opds_feed(payload, self.base_url)[1], next_link

        stale = None
        if cache:
            with self._book_cache_lock:
                cached = self._book_cache.get(path)
                if cached and (len(cached[1]) >= needed or cached[3]):
                    if monotonic() - cached[0] < 300:
                        self._book_cache.move_to_end(path)
                        return cached[1][:needed], cached[2] if cached[3] else None
                    # Keep an expired, bounded prefix as a fallback. An
                    # incomplete prefix cannot answer a deeper page safely.
                    if monotonic() - cached[0] < 3600:
                        stale = (cached[1][:needed], cached[2] if cached[3] else None)
        for attempt in range(2):
            try:
                # The first page needs only one book beyond the visible grid.
                # Waiting for all twenty entries delays display when a large
                # upstream feed pauses after the first thirteen books.
                books, next_link, complete = self._stream_book_feed(path, needed)
                if cache and len(books) <= 240:
                    with self._book_cache_lock:
                        previous = self._book_cache.get(path)
                        if not previous or monotonic() - previous[0] >= 300 or len(books) >= len(previous[1]):
                            self._book_cache[path] = (monotonic(), books, next_link, complete)
                        self._book_cache.move_to_end(path)
                        while len(self._book_cache) > 16:
                            self._book_cache.popitem(last=False)
                return books, next_link
            except (HTTPError, URLError, OSError, TimeoutError, http.client.HTTPException) as exc:
                if stale and retryable_source_error(exc):
                    return stale
                if attempt or not retryable_source_error(exc):
                    raise SourceUnavailable("Flibusta did not respond; please retry") from exc

    def _stream_book_feed(self, path: str, needed: int) -> tuple[list[Book], str | None, bool]:
        request = Request(urljoin(self.base_url + "/", path.lstrip("/")), headers={"User-Agent": "BookRelay/0.1"})
        parser = ET.XMLPullParser(events=("start", "end"))
        root = None
        next_link = None
        books: list[Book] = []
        received = 0
        complete = False
        try:
            with urlopen(request, timeout=self.timeout) as response:
                while len(books) < needed:
                    # read(n) waits for n bytes, even if the first page is already available.
                    chunk = response.read1(8192)
                    if not chunk:
                        parser.close()
                        complete = True
                        break
                    received += len(chunk)
                    if received > 25 * 1024 * 1024:
                        raise SourceUnavailable("Flibusta response exceeds 25 MiB")
                    parser.feed(chunk)
                    for event, element in parser.read_events():
                        if event == "start" and root is None:
                            root = element
                            if root.tag != f"{ATOM}feed":
                                raise SourceUnavailable("Flibusta returned an invalid catalog")
                        elif event == "end" and root is not None:
                            if element is root:
                                complete = True
                            elif element.tag == f"{ATOM}link" and element in root and element.get("rel") == "next":
                                next_link = element.get("href")
                            elif element.tag == f"{ATOM}entry" and element in root:
                                book = parse_book_entry(element, self.base_url)
                                if book:
                                    books.append(book)
                                root.remove(element)
                    if len(books) >= needed:
                        break
        except ET.ParseError as exc:
            raise SourceUnavailable("Flibusta returned an invalid catalog") from exc
        if root is None:
            raise SourceUnavailable("Flibusta returned an invalid catalog")
        return books, next_link, complete

    def _paged_books(self, path: str, page: int, size: int = 6, cache: bool = False) -> tuple[list[Book], bool]:
        start = (page - 1) * size
        target = start + size
        books_seen: list[Book] = []
        visited: set[str] = set()
        while path and len(books_seen) < target + 1:
            if path in visited or len(visited) >= 100:
                raise SourceUnavailable("Flibusta returned invalid pagination")
            visited.add(path)
            books, next_link = self._book_feed(path, target + 1 - len(books_seen), cache)
            books_seen.extend(books)
            path = self._next_page_path(path, next_link) if next_link else None
        return books_seen[start:start + size], len(books_seen) > start + size or bool(path)

    def _next_page_path(self, current: str, href: str) -> str:
        # OPDS links are supplied by the source. Resolve relative links against
        # the current feed, but never follow one to another host or endpoint.
        try:
            link = urlsplit(href)
            resolved = urlsplit(urljoin(urljoin(self.base_url + "/", current.lstrip("/")), href))
        except ValueError as exc:
            raise SourceUnavailable("Flibusta returned invalid pagination") from exc
        origin = urlsplit(self.base_url)
        if (not href or link.fragment or resolved.fragment or
                (resolved.scheme, resolved.netloc) != (origin.scheme, origin.netloc) or
                not resolved.path.startswith("/opds/") or
                any(ord(char) < 32 for char in href)):
            raise SourceUnavailable("Flibusta returned invalid pagination")
        return resolved.path + ("?" + resolved.query if resolved.query else "")

    def categories(self) -> list[dict[str, str]]:
        if self._snapshot_categories is not None:
            return [item.copy() for item in self._snapshot_categories]
        sections, _, _ = parse_opds_feed(self._catalog_feed("/opds/genres"), self.base_url)
        return sections

    def subcategories(self, category: str) -> list[dict[str, str]]:
        if not re.fullmatch(r"/opds/genres/[^/?#]+", category):
            raise ValueError("unknown category")
        if category in self._snapshot_subcategories:
            return [item.copy() for item in self._snapshot_subcategories[category]]
        sections, _, _ = parse_opds_feed(self._catalog_feed(category), self.base_url)
        return sections

    def catalog_books(self, category: str, subcategory: str, page: int = 1, size: int = 6) -> tuple[list[Book], bool]:
        if not re.fullmatch(r"/opds/genres/[^/?#]+", category) or not re.fullmatch(re.escape(category) + r"/[^/?#]+", subcategory):
            raise ValueError("unknown subcategory")
        return self._paged_books(subcategory, page, size, cache=True)

    def cover_path(self, book_id: str, cover_url: str) -> str:
        """Only cover paths tied to this book on the configured OPDS host may be fetched."""
        try:
            origin, cover = urlsplit(self.base_url), urlsplit(cover_url)
        except ValueError as exc:
            raise ValueError("invalid cover URL") from exc
        if not re.fullmatch(r"[0-9]+", book_id) or (cover.scheme, cover.netloc) != (origin.scheme, origin.netloc):
            raise ValueError("invalid cover URL")
        # Some EPUB thumbnails live in nested paths such as
        # /i/94/756694/OEBPS/Images/x0000.jpg.jpg. Validate every segment
        # separately; the book id and origin must still match.
        image_folder = re.fullmatch(rf"/i/[0-9]{{1,2}}/{re.escape(book_id)}/(.+)", cover.path)
        direct_cover = re.fullmatch(rf"/covers/{re.escape(book_id)}\.(?:jpg|jpeg|png)", cover.path, re.I)
        parts = image_folder.group(1).split("/") if image_folder else []
        if (cover.query or cover.fragment or not (image_folder or direct_cover) or
                (image_folder and (len(parts) > 6 or len(cover.path) > 256 or
                                   any(part in (".", "..") or
                                       not re.fullmatch(r"[A-Za-z0-9._-]{1,96}", part)
                                       for part in parts)))):
            raise ValueError("invalid cover path")
        return cover.path

    def download_cover(self, book_id: str, path: str) -> bytes:
        self.cover_path(book_id, urljoin(self.base_url + "/", path.lstrip("/")))
        payload = self._get(path)
        if len(payload) > 8 * 1024 * 1024 or not (payload.startswith(b"\xff\xd8\xff") or payload.startswith(b"\x89PNG\r\n\x1a\n")):
            raise ValueError("invalid cover image")
        return payload

    def details(self, book_id: str) -> Book:
        page = self._get(f"/b/{book_id}").decode("utf-8", "replace")
        books = parse_search_page(f'<article class="book-card">{page}</article>', self.base_url)
        if books:
            return books[0]
        title_match = re.search(r"<title>(.*?)</title>", page, re.I | re.S)
        return Book(id=book_id, title=clean_text(title_match.group(1)) if title_match else book_id)

    def download(self, book_id: str) -> bytes:
        payload = self._get(f"/b/{book_id}/epub", max_bytes=MAX_EPUB_BYTES)
        validate_epub(payload)
        return payload
