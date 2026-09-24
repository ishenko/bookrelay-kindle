import tempfile
import unittest
import zipfile
from datetime import datetime, timedelta
from io import BytesIO
from pathlib import Path
from unittest.mock import patch

import httpx

from bookrelay.delivery import build_epub_email
from bookrelay.main import RateLimiter, create_app
from bookrelay.models import Book
from bookrelay.pairing import PairingStore
from bookrelay.source.flibusta import FlibustaSource, SourceUnavailable


def make_epub():
    output = BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    return output.getvalue()


class FakeSource:
    def search(self, query, page=1, category=None):
        return [Book(id="123", title="A Book", author="An Author")]

    def details(self, book_id):
        return Book(id=book_id, title="A Book", author="An Author")

    def download(self, book_id):
        return make_epub()

    def categories(self):
        return [{"id": "new", "title": "New"}]

    def subcategories(self, category):
        if category != "new":
            raise ValueError("unknown category")
        return [{"id": "new/fantasy", "title": "Fantasy"}]

    def catalog_books(self, category, subcategory, page=1, size=6):
        if (category, subcategory) != ("new", "new/fantasy"):
            raise ValueError("unknown subcategory")
        return ([Book(id="123", title="A Book", author="An Author", year=2022)] if page == 1 else []), False


class FakeMailer:
    def __init__(self):
        self.sent = []

    def send(self, recipient, filename, payload):
        self.sent.append((recipient, filename, payload))


class RateLimiterTests(unittest.TestCase):
    def test_expired_clients_are_removed_without_resetting_longer_limits(self):
        limiter = RateLimiter()
        with patch("bookrelay.main.monotonic", return_value=0):
            self.assertTrue(limiter.allow("delivery:reader", 1, 3600))
            self.assertTrue(limiter.allow("categories:expired", 1, 60))
        with patch("bookrelay.main.monotonic", return_value=61):
            for i in range(254):
                self.assertTrue(limiter.allow(f"categories:new-{i}", 1, 60))
            self.assertNotIn("categories:expired", limiter.events)
            self.assertFalse(limiter.allow("delivery:reader", 1, 3600))
        with patch("bookrelay.main.monotonic", return_value=3601):
            self.assertTrue(limiter.allow("delivery:reader", 1, 3600))


class ApiTests(unittest.IsolatedAsyncioTestCase):
    async def test_source_outage_returns_service_unavailable(self):
        class OfflineSource(FakeSource):
            def subcategories(self, category):
                raise SourceUnavailable("Flibusta did not respond; please retry")

        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=OfflineSource(),
                             mailer=FakeMailer(), pairing_admin_key="test-owner-key")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                token = (await client.post("/v1/pair/claim", json={"code": start.json()["code"]})).json()["token"]
                response = await client.get("/v1/subcategories", params={"category": "new"},
                                            headers={"Authorization": f"Bearer {token}"})
                self.assertEqual(response.status_code, 503)
                self.assertIn("Flibusta did not respond", response.json()["detail"])

    async def test_catalog_cover_is_served_through_authenticated_relay(self):
        class CoverSource(FlibustaSource):
            def __init__(self):
                super().__init__()
                self.cover_requests = []

            def catalog_books(self, category, subcategory, page=1, size=6):
                return [Book(id="451198", title="A Book", cover_url="https://flibusta.is/i/98/451198/img_12"),
                        Book(id="123", title="Another Book", cover_url="https://flibusta.is/covers/123.jpg"),
                        Book(id="756694", title="Nested Book", cover_url="https://flibusta.is/i/94/756694/OEBPS/Images/x0000.jpg.jpg"),
                        Book(id="326334", title="Archive Book", cover_url="https://flibusta.is/ib/5/195905/cover.jpg")], False

            def _get(self, path):
                self.cover_requests.append(path)
                return b"\xff\xd8\xfftest-jpeg"

        with tempfile.TemporaryDirectory() as tmp:
            source = CoverSource()
            app = create_app(Path(tmp) / "relay.sqlite3", source=source, mailer=FakeMailer(), pairing_admin_key="test-owner-key")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                token = (await client.post("/v1/pair/claim", json={"code": start.json()["code"]})).json()["token"]
                headers = {"Authorization": f"Bearer {token}"}
                catalog = await client.get("/v1/catalog/books", params={"category": "x", "subcategory": "y"}, headers=headers)
                cover_url = catalog.json()["items"][0]["cover_url"]
                self.assertTrue(cover_url.startswith("/v1/books/451198/cover?path="))
                self.assertNotIn("flibusta.is", cover_url)
                self.assertEqual((await client.get(cover_url)).status_code, 401)
                cover = await client.get(cover_url, headers=headers)
                self.assertEqual(cover.status_code, 200)
                self.assertEqual(cover.content, b"\xff\xd8\xfftest-jpeg")
                self.assertEqual(cover.headers["content-type"], "image/jpeg")
                self.assertEqual(source.cover_requests, ["/i/98/451198/img_12"])
                direct_url = catalog.json()["items"][1]["cover_url"]
                self.assertEqual(direct_url, "/v1/books/123/cover?path=%2Fcovers%2F123.jpg")
                direct_cover = await client.get(direct_url, headers=headers)
                self.assertEqual(direct_cover.status_code, 200)
                self.assertEqual(source.cover_requests, ["/i/98/451198/img_12", "/covers/123.jpg"])
                nested_url = catalog.json()["items"][2]["cover_url"]
                self.assertIn("/v1/books/756694/cover?path=", nested_url)
                self.assertEqual((await client.get(nested_url, headers=headers)).status_code, 200)
                self.assertEqual(source.cover_requests[-1], "/i/94/756694/OEBPS/Images/x0000.jpg.jpg")
                archive_url = catalog.json()["items"][3]["cover_url"]
                self.assertIn("path=%2Fib%2F5%2F195905%2Fcover.jpg&sig=", archive_url)
                self.assertEqual((await client.get(archive_url, headers=headers)).status_code, 200)
                self.assertEqual(source.cover_requests[-1], "/ib/5/195905/cover.jpg")
                for tampered in (archive_url.replace("326334", "326335"),
                                 archive_url.replace("195905", "195906"),
                                 "/v1/books/326334/cover?path=%2Fib%2F5%2F195905%2Fcover.jpg"):
                    self.assertEqual((await client.get(tampered, headers=headers)).status_code, 404)
                self.assertEqual(len(source.cover_requests), 4)
                rejected = await client.get("/v1/books/451198/cover", params={"path": "//elsewhere.test/secret"}, headers=headers)
                self.assertEqual(rejected.status_code, 404)
                self.assertEqual(len(source.cover_requests), 4)

    async def test_pair_page_shows_remaining_time_from_server_clock(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer(), pairing_admin_key="test-owner-key")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                page = await client.get("/pair")
                self.assertEqual(page.status_code, 200)
                self.assertIn("id='remaining'", page.text)
                self.assertIn("id='lifetime'", page.text)
                self.assertIn("performance.now()", page.text)
                started = await client.post("/v1/pair/start", json={
                    "device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key",
                })
                self.assertEqual(started.status_code, 200)
                data = started.json()
                duration = datetime.fromisoformat(data["expires_at"]) - datetime.fromisoformat(data["server_time"])
                self.assertGreater(duration, timedelta(minutes=9, seconds=55))
                self.assertLessEqual(duration, timedelta(minutes=10))

    async def test_catalog_navigation_and_empty_page(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer(), pairing_admin_key="test-owner-key")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                token = (await client.post("/v1/pair/claim", json={"code": start.json()["code"]})).json()["token"]
                headers = {"Authorization": f"Bearer {token}"}
                categories = await client.get("/v1/categories", headers=headers)
                subcategories = await client.get("/v1/subcategories", params={"category": "new"}, headers=headers)
                books = await client.get("/v1/catalog/books", params={"category": "new", "subcategory": "new/fantasy", "page": 1}, headers=headers)
                empty = await client.get("/v1/catalog/books", params={"category": "new", "subcategory": "new/fantasy", "page": 2}, headers=headers)
                self.assertEqual(categories.json(), [{"id": "new", "title": "New"}])
                self.assertEqual(subcategories.json(), [{"id": "new/fantasy", "title": "Fantasy"}])
                self.assertEqual(books.json()["items"][0]["year"], 2022)
                self.assertEqual(empty.json()["items"], [])
                self.assertFalse(empty.json()["has_next"])

    async def test_pair_search_and_authenticated_delivery(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer(), pairing_admin_key="test-owner-key", delivery_enabled=True)
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                self.assertEqual(start.status_code, 200)
                code = start.json()["code"]
                claimed = await client.post("/v1/pair/claim", json={"code": code})
                self.assertEqual(claimed.status_code, 200)
                self.assertEqual(claimed.json()["kindle_email"], "reader@kindle.com")
                token = claimed.json()["token"]
                auth = {"Authorization": f"Bearer {token}"}
                search = await client.get("/v1/search", params={"q": "book", "category": "fantasy"}, headers=auth)
                self.assertEqual(search.status_code, 200)
                self.assertEqual(search.json()["category"], "fantasy")
                delivery = await client.post(
                    "/v1/deliveries",
                    headers={"Authorization": f"Bearer {token}"},
                    json={"book_id": "123", "title": "A Book"},
                )
                self.assertEqual(delivery.status_code, 202)
                job = await client.get(f"/v1/deliveries/{delivery.json()['id']}", headers={"Authorization": f"Bearer {token}"})
                self.assertEqual(job.json()["status"], "sent")

    async def test_pair_start_requires_owner_key_and_stores_email_from_pairing_page(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer(), pairing_admin_key="test-owner-key")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                denied = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "wrong"})
                self.assertEqual(denied.status_code, 403)
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                self.assertEqual(start.status_code, 200)
                code = start.json()["code"]
                extra_fields = await client.post("/v1/pair/claim", json={"code": code, "kindle_email": "attacker@example.com"})
                self.assertEqual(extra_fields.status_code, 422)
                claimed = await client.post("/v1/pair/claim", json={"code": code})
                self.assertEqual(claimed.json()["kindle_email"], "reader@kindle.com")

    async def test_pairing_works_without_default_kindle_email(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer(), pairing_admin_key="test-owner-key")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                self.assertEqual(start.status_code, 200)

    async def test_two_devices_deliver_to_their_own_kindle_addresses(self):
        with tempfile.TemporaryDirectory() as tmp:
            mailer = FakeMailer()
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=mailer, pairing_admin_key="test-owner-key", delivery_enabled=True)
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                tokens = []
                for device_id, email in (("pw11", "first@kindle.com"), ("pw12", "second@kindle.com")):
                    start = await client.post("/v1/pair/start", json={"device_id": device_id, "kindle_email": email, "admin_key": "test-owner-key"})
                    code = start.json()["code"]
                    tokens.append((await client.post("/v1/pair/claim", json={"code": code})).json()["token"])

                for token in tokens:
                    response = await client.post("/v1/deliveries", headers={"Authorization": f"Bearer {token}"}, json={"book_id": "123"})
                    self.assertEqual(response.status_code, 202)

                self.assertEqual([item[0] for item in mailer.sent], ["first@kindle.com", "second@kindle.com"])

    async def test_device_can_change_delivery_email_with_its_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            mailer = FakeMailer()
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=mailer,
                             pairing_admin_key="test-owner-key", delivery_enabled=True)
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                start = await client.post("/v1/pair/start", json={"device_id": "pw12", "kindle_email": "old@kindle.com", "admin_key": "test-owner-key"})
                claim = await client.post("/v1/pair/claim", json={"code": start.json()["code"].lower()})
                token = claim.json()["token"]
                headers = {"Authorization": f"Bearer {token}"}
                self.assertEqual((await client.put("/v1/devices/me", json={"kindle_email": "new@kindle.com"})).status_code, 401)
                self.assertEqual((await client.put("/v1/devices/me", json={"kindle_email": "invalid"}, headers=headers)).status_code, 422)
                changed = await client.put("/v1/devices/me", json={"kindle_email": "new@kindle.com"}, headers=headers)
                self.assertEqual(changed.json()["kindle_email"], "new@kindle.com")
                self.assertEqual(app.state.pairing.authenticate(token)["kindle_email"], "new@kindle.com")
                sent = await client.post("/v1/deliveries", headers=headers, json={"book_id": "123"})
                self.assertEqual(sent.status_code, 202)
                self.assertEqual(mailer.sent[-1][0], "new@kindle.com")

    async def test_delivery_requires_pairing_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer())
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                response = await client.post("/v1/deliveries", json={"book_id": "123", "title": "A Book"})
                self.assertEqual(response.status_code, 401)

    async def test_catalog_requires_pairing_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.sqlite3", source=FakeSource(), mailer=FakeMailer())
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test") as client:
                response = await client.get("/v1/search", params={"q": "book"})
                self.assertEqual(response.status_code, 401)


if __name__ == "__main__":
    unittest.main()
