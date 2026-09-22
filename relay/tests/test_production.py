import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

import httpx

from bookrelay.main import create_app
from bookrelay.pairing import PairingStore, utc_now
from test_api import FakeMailer, FakeSource


class ProductionTests(unittest.IsolatedAsyncioTestCase):
    async def test_owner_key_and_delivery_switch(self):
        with tempfile.TemporaryDirectory() as tmp:
            mailer = FakeMailer()
            app = create_app(Path(tmp) / "relay.db", source=FakeSource(), mailer=mailer,
                             pairing_admin_key="owner-test-secret", delivery_enabled=False)
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="https://test") as c:
                code = (await c.post("/v1/pair/start", json={"device_id": "test"})).json()["code"]
                body = {"code": code, "kindle_email": "reader@kindle.com"}
                self.assertEqual((await c.post("/v1/pair/claim", json=body)).status_code, 403)
                body["admin_key"] = "wrong"
                self.assertEqual((await c.post("/v1/pair/claim", json=body)).status_code, 403)
                body["admin_key"] = "owner-test-secret"
                self.assertEqual((await c.post("/v1/pair/claim", json=body)).status_code, 200)
                token = (await c.get("/v1/pair/status/" + code)).json()["token"]
                response = await c.post("/v1/deliveries", json={"book_id": "123"}, headers={"Authorization": "Bearer " + token})
                self.assertEqual(response.status_code, 503)
                self.assertEqual(mailer.sent, [])
                self.assertFalse((await c.get("/healthz")).json()["delivery_enabled"])

    async def test_unconfigured_pairing_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Path(tmp) / "relay.db", source=FakeSource(), mailer=FakeMailer(), pairing_admin_key="")
            async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="https://test") as c:
                r = await c.post("/v1/pair/claim", json={"code": "ABCD", "kindle_email": "r@kindle.com"})
                self.assertEqual(r.status_code, 503)

    async def test_claimed_code_does_not_expose_token_after_expiry(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "relay.db")
            code = store.start_pairing("test").code
            store.claim(code, "r@kindle.com")
            with store._connect() as conn:
                conn.execute("UPDATE pairings SET expires_at = ? WHERE code = ?", ((utc_now() - timedelta(seconds=1)).isoformat(), code))
            self.assertEqual(store.status(code), {"status": "expired"})
