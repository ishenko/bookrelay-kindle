import html
import re
from urllib.parse import quote_plus, urljoin
from urllib.request import Request, urlopen

from ..delivery import validate_epub
from ..models import Book


TAG_RE = re.compile(r"<[^>]+>")


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

    def search(self, query: str, page: int = 1) -> list[Book]:
        suffix = f"/booksearch?ask={quote_plus(query)}&page={page}"
        return parse_search_page(self._get(suffix).decode("utf-8", "replace"), self.base_url)

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

    @staticmethod
    def categories() -> list[dict[str, str]]:
        return [
            {"id": "new", "title": "Новые книги"},
            {"id": "popular", "title": "Популярное"},
            {"id": "fantasy", "title": "Фантастика"},
            {"id": "detective", "title": "Детектив"},
            {"id": "novel", "title": "Романы"},
        ]
