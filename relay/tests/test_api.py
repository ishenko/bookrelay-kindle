import tempfile
import unittest
import zipfile
from io import BytesIO
from pathlib import Path

import httpx

from bookrelay.delivery import build_epub_email
from bookrelay.main import create_app
from bookrelay.models import Book
from bookrelay.pairing import PairingStore


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


class ApiTests(unittest.IsolatedAsyncioTestCase):
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
