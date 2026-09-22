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
    def search(self, query, page=1):
        return [Book(id="123", title="A Book", author="An Author")]

    def details(self, book_id):
        return Book(id=book_id, title="A Book", author="An Author")

    def download(self, book_id):
        return make_epub()

    def categories(self):
        return [{"id": "new", "title": "New"}]


class FakeMailer:
    def __init__(self):
        self.sent = []

    def send(self, recipient, filename, payload):
        self.sent.append((recipient, filename, payload))


class ApiTests(unittest.IsolatedAsyncioTestCase):
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
                self.assertEqual((await client.get("/v1/search", params={"q": "book"}, headers=auth)).status_code, 200)
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
