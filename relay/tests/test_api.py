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
                start = await client.post("/v1/pair/start", json={"device_id": "pw12"})
                self.assertEqual(start.status_code, 200)
                code = start.json()["code"]
                claimed = await client.post("/v1/pair/claim", json={"code": code, "kindle_email": "reader@kindle.com", "admin_key": "test-owner-key"})
                self.assertEqual(claimed.status_code, 200)
                token = (await client.get(f"/v1/pair/status/{code}")).json()["token"]
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
