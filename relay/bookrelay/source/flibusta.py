import html
import re
from xml.etree import ElementTree as ET
from urllib.parse import quote_plus, urljoin
from urllib.request import Request, urlopen

from ..delivery import validate_epub
from ..models import Book


TAG_RE = re.compile(r"<[^>]+>")
ATOM = "{http://www.w3.org/2005/Atom}"
DC = "{http://purl.org/dc/terms/}"


def parse_opds_feed(payload: bytes, base_url: str) -> tuple[list[dict[str, str]], list[Book], bool]:
    root = ET.fromstring(payload)
    sections, books = [], []
    has_next = any(link.get("rel") == "next" for link in root.findall(f"{ATOM}link"))
    for entry in root.findall(f"{ATOM}entry"):
        title = (entry.findtext(f"{ATOM}title") or "").strip()
        links = entry.findall(f"{ATOM}link")
        section = next((link for link in links if "opds-catalog" in link.get("type", "") and link.get("rel", "") in ("", "subsection")), None)
        if section is not None:
            sections.append({"id": section.get("href", ""), "title": title})
            continue
        acquisition = next((link for link in links if "/b/" in link.get("href", "") and "acquisition" in link.get("rel", "")), None)
        if acquisition is None:
            continue
        match = re.search(r"/b/([0-9]+)", acquisition.get("href", ""))
        if not match:
            continue
        cover = next((link.get("href", "") for link in links if link.get("rel") in ("http://opds-spec.org/thumbnail", "http://opds-spec.org/image")), "")
        issued = entry.findtext(f"{DC}issued") or ""
        content = entry.findtext(f"{ATOM}content") or ""
        books.append(Book(id=match.group(1), title=title,
                          author=", ".join(author.findtext(f"{ATOM}name") or "" for author in entry.findall(f"{ATOM}author")),
                          cover_url=urljoin(base_url, cover) if cover else "",
                          description=clean_text(content)[:2000],
                          year=int(issued[:4]) if re.fullmatch(r"\d{4}", issued[:4]) else None))
    return sections, books, has_next


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

    def _get(self, path: str) -> bytes:
        request = Request(urljoin(self.base_url + "/", path.lstrip("/")), headers={"User-Agent": "BookRelay/0.1"})
        with urlopen(request, timeout=self.timeout) as response:
            payload = response.read(25 * 1024 * 1024 + 1)
            if len(payload) > 25 * 1024 * 1024:
                raise ValueError("source response exceeds 25 MiB")
            return payload

    def search(self, query: str, page: int = 1, category: str | None = None) -> list[Book]:
        return self.search_page(query, page)[0]

    def search_page(self, query: str, page: int = 1, size: int = 6) -> tuple[list[Book], bool]:
        suffix = f"/opds/search?searchTerm={quote_plus(query.strip())}"
        return self._paged_books(suffix, page, size)

    def _paged_books(self, path: str, page: int, size: int = 6) -> tuple[list[Book], bool]:
        start = (page - 1) * size
        target = start + size + 1
        books_seen: list[Book] = []
        visited: set[str] = set()
        while path and len(books_seen) < target:
            if path in visited:
                raise ValueError("cyclic OPDS pagination")
            visited.add(path)
            payload = self._get(path)
            root = ET.fromstring(payload)
            next_link = next((link.get("href") for link in root.findall(f"{ATOM}link") if link.get("rel") == "next"), None)
            _, books, _ = parse_opds_feed(payload, self.base_url)
            books_seen.extend(books)
            path = next_link
        return books_seen[start:start + size], len(books_seen) > start + size or bool(path)

    def categories(self) -> list[dict[str, str]]:
        sections, _, _ = parse_opds_feed(self._get("/opds/genres"), self.base_url)
        return sections

    def subcategories(self, category: str) -> list[dict[str, str]]:
        if category not in {item["id"] for item in self.categories()}:
            raise ValueError("unknown category")
        sections, _, _ = parse_opds_feed(self._get(category), self.base_url)
        return sections

    def catalog_books(self, category: str, subcategory: str, page: int = 1, size: int = 6) -> tuple[list[Book], bool]:
        if subcategory not in {item["id"] for item in self.subcategories(category)}:
            raise ValueError("unknown subcategory")
        return self._paged_books(subcategory, page, size)

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
