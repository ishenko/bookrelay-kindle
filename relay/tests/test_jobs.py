import tempfile
import unittest
import zipfile
from io import BytesIO
from pathlib import Path

from bookrelay.jobs import DeliveryService, JobStore


def make_epub():
    output = BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    return output.getvalue()


class FakeSource:
    def download(self, book_id):
        return make_epub()


class FakeMailer:
    def __init__(self):
        self.sent = []

    def send(self, recipient, filename, payload):
        self.sent.append((recipient, filename, payload))


class DeliveryServiceTests(unittest.TestCase):
    def test_delivery_moves_from_queued_to_sent(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = JobStore(Path(tmp) / "jobs.sqlite3")
            mailer = FakeMailer()
            service = DeliveryService(store, FakeSource(), mailer)
            job = service.enqueue("123", "kindle@example.com", "A Book")

            self.assertEqual(store.get(job["id"])["status"], "queued")
            service.run(job["id"])

            result = store.get(job["id"])
            self.assertEqual(result["status"], "sent")
            self.assertEqual(mailer.sent[0][0], "kindle@example.com")
            self.assertEqual(mailer.sent[0][1], "A_Book.epub")

    def test_delivery_records_failure_without_losing_job(self):
        class FailingSource:
            def download(self, book_id):
                raise RuntimeError("source unavailable")

        with tempfile.TemporaryDirectory() as tmp:
            store = JobStore(Path(tmp) / "jobs.sqlite3")
            service = DeliveryService(store, FailingSource(), FakeMailer())
            job = service.enqueue("123", "kindle@example.com", "A Book")
            service.run(job["id"])
            result = store.get(job["id"])
            self.assertEqual(result["status"], "failed")
            self.assertIn("source unavailable", result["error"])


if __name__ == "__main__":
    unittest.main()
