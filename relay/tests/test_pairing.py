import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

from bookrelay.pairing import PairingStore


class PairingStoreTests(unittest.TestCase):
    def test_code_is_claimed_once_and_status_returns_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            pairing = store.start_pairing(device_id="device-1", ttl=timedelta(minutes=5))

            self.assertEqual(store.status(pairing.code)["status"], "pending")
            claim = store.claim(pairing.code, "reader@example.com")
            self.assertEqual(claim["kindle_email"], "reader@example.com")
            self.assertTrue(claim["token"])
            self.assertEqual(store.status(pairing.code)["status"], "claimed")
            with self.assertRaises(ValueError):
                store.claim(pairing.code, "other@example.com")

    def test_expired_code_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            pairing = store.start_pairing(device_id="device-1", ttl=timedelta(seconds=-1))
            with self.assertRaises(ValueError):
                store.claim(pairing.code, "reader@example.com")

    def test_revoked_token_cannot_authenticate(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            pairing = store.start_pairing(device_id="device-1")
            claim = store.claim(pairing.code, "reader@example.com")

            self.assertIsNotNone(store.authenticate(claim["token"]))
            store.revoke(claim["token"])
            self.assertIsNone(store.authenticate(claim["token"]))


if __name__ == "__main__":
    unittest.main()
