import tempfile
import unittest
from datetime import timedelta
from pathlib import Path

from bookrelay.pairing import PairingStore


class PairingStoreTests(unittest.TestCase):
    def test_code_is_claimed_once_and_status_does_not_return_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            pairing = store.start_pairing(device_id="device-1", kindle_email="reader@example.com", ttl=timedelta(minutes=5))

            self.assertEqual(store.status(pairing.code)["status"], "pending")
            claim = store.claim("  " + pairing.code.lower() + "  ")
            self.assertEqual(claim["kindle_email"], "reader@example.com")
            self.assertTrue(claim["token"])
            status = store.status(pairing.code)
            self.assertEqual(status["status"], "claimed")
            self.assertNotIn("token", status)
            with self.assertRaises(ValueError):
                store.claim(pairing.code)

    def test_expired_code_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            pairing = store.start_pairing(device_id="device-1", kindle_email="reader@example.com", ttl=timedelta(seconds=-1))
            with self.assertRaises(ValueError):
                store.claim(pairing.code)

    def test_revoked_token_cannot_authenticate(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            pairing = store.start_pairing(device_id="device-1", kindle_email="reader@example.com")
            claim = store.claim(pairing.code)

            self.assertIsNotNone(store.authenticate(claim["token"]))
            store.revoke(claim["token"])
            self.assertIsNone(store.authenticate(claim["token"]))

    def test_two_pairing_codes_keep_different_kindle_emails(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            first = store.start_pairing("pw11", "first@kindle.com")
            second = store.start_pairing("pw12", "second@kindle.com")

            first_claim = store.claim(first.code)
            second_claim = store.claim(second.code)

            self.assertEqual(store.authenticate(first_claim["token"])["kindle_email"], "first@kindle.com")
            self.assertEqual(store.authenticate(second_claim["token"])["kindle_email"], "second@kindle.com")

    def test_pairing_rejects_invalid_kindle_email_before_creating_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = PairingStore(Path(tmp) / "pairing.sqlite3")
            with self.assertRaisesRegex(ValueError, "invalid Kindle Email"):
                store.start_pairing("device-1", "not-an-email")


if __name__ == "__main__":
    unittest.main()
