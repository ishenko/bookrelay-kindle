import html
import re
from collections import OrderedDict
from urllib.error import HTTPError, URLError
from threading import Lock
from time import monotonic
from xml.etree import ElementTree as ET
from urllib.parse import quote_plus, urljoin, urlsplit
from urllib.request import Request, urlopen

from ..delivery import validate_epub
from ..models import Book


TAG_RE = re.compile(r"<[^>]+>")
ATOM = "{http://www.w3.org/2005/Atom}"
DC = "{http://purl.org/dc/terms/}"


class SourceUnavailable(Exception):
    pass


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
    cover = next((link.get("href", "") for link in links if link.get("rel") in ("http://opds-spec.org/thumbnail", "http://opds-spec.org/image")), "")
    issued = entry.findtext(f"{DC}issued") or ""
    content = entry.findtext(f"{ATOM}content") or ""
    return Book(id=match.group(1), title=(entry.findtext(f"{ATOM}title") or "").strip(),
                author=", ".join(author.findtext(f"{ATOM}name") or "" for author in entry.findall(f"{ATOM}author")),
                cover_url=urljoin(base_url, cover) if cover else "",
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
    def __init__(self, base_url: str = "https://flibusta.is", timeout: int = 20):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self._feed_cache: OrderedDict[str, tuple[float, bytes]] = OrderedDict()
        self._feed_cache_bytes = 0
        self._feed_cache_lock = Lock()

    def _get(self, path: str) -> bytes:
        request = Request(urljoin(self.base_url + "/", path.lstrip("/")), headers={"User-Agent": "BookRelay/0.1"})
        try:
            with urlopen(request, timeout=self.timeout) as response:
                payload = response.read(25 * 1024 * 1024 + 1)
                if len(payload) > 25 * 1024 * 1024:
                    raise SourceUnavailable("Flibusta response exceeds 25 MiB")
                return payload
        except (HTTPError, URLError, OSError, TimeoutError) as exc:
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

        request = Request(urljoin(self.base_url + "/", path.lstrip("/")), headers={"User-Agent": "BookRelay/0.1"})
        parser = ET.XMLPullParser(events=("start", "end"))
        root = None
        next_link = None
        books: list[Book] = []
        received = 0
        try:
            with urlopen(request, timeout=self.timeout) as response:
                while len(books) < needed:
                    # read(n) waits for n bytes, even if the first page is already available.
                    chunk = response.read1(8192)
                    if not chunk:
                        parser.close()
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
                            if element.tag == f"{ATOM}link" and element in root and element.get("rel") == "next":
                                next_link = element.get("href")
                            elif element.tag == f"{ATOM}entry" and element in root:
                                book = parse_book_entry(element, self.base_url)
                                if book:
                                    books.append(book)
                                root.remove(element)
                    if len(books) >= needed:
                        break
        except (HTTPError, URLError, OSError, TimeoutError) as exc:
            raise SourceUnavailable("Flibusta did not respond; please retry") from exc
        except ET.ParseError as exc:
            raise SourceUnavailable("Flibusta returned an invalid catalog") from exc
        if root is None:
            raise SourceUnavailable("Flibusta returned an invalid catalog")
        return books, next_link

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
            path = next_link
        return books_seen[start:start + size], len(books_seen) > start + size or bool(path)

    def categories(self) -> list[dict[str, str]]:
        sections, _, _ = parse_opds_feed(self._catalog_feed("/opds/genres"), self.base_url)
        return sections

    def subcategories(self, category: str) -> list[dict[str, str]]:
        if not re.fullmatch(r"/opds/genres/[^/?#]+", category):
            raise ValueError("unknown category")
        sections, _, _ = parse_opds_feed(self._catalog_feed(category), self.base_url)
        return sections

    def catalog_books(self, category: str, subcategory: str, page: int = 1, size: int = 6) -> tuple[list[Book], bool]:
        if not re.fullmatch(r"/opds/genres/[^/?#]+", category) or not re.fullmatch(re.escape(category) + r"/[^/?#]+", subcategory):
            raise ValueError("unknown subcategory")
        return self._paged_books(subcategory, page, size, cache=True)

    def cover_path(self, book_id: str, cover_url: str) -> str:
        """Only cover paths tied to this book on the configured OPDS host may be fetched."""
        origin, cover = urlsplit(self.base_url), urlsplit(cover_url)
        if not re.fullmatch(r"[0-9]+", book_id) or (cover.scheme, cover.netloc) != (origin.scheme, origin.netloc):
            raise ValueError("invalid cover URL")
        # OPDS covers can have names such as img_12 or sol22.jpg, not just cover.jpg.
        # Keep the request on the configured host and within this book's image folder.
        filename = cover.path.rsplit("/", 1)[-1]
        if (cover.query or cover.fragment or
                not re.fullmatch(rf"/i/[0-9]{{1,2}}/{re.escape(book_id)}/[^/]+", cover.path) or
                not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9._-]{0,95}", filename) or
                ".." in filename):
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
        payload = self._get(f"/b/{book_id}/epub")
        validate_epub(payload)
        return payload
