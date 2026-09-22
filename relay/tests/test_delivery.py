import unittest
import zipfile
from email import policy
from email.parser import BytesParser
from io import BytesIO

from bookrelay.delivery import build_epub_email, validate_epub


def make_epub():
    output = BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        archive.writestr("OEBPS/content.xhtml", "<html><body>Book</body></html>")
    return output.getvalue()


class DeliveryTests(unittest.TestCase):
    def test_validates_epub_and_builds_kindle_email(self):
        payload = make_epub()
        validate_epub(payload)
        message = BytesParser(policy=policy.default).parsebytes(
            build_epub_email(
                sender="relay@example.com",
                recipient="kindle_123@kindle.com",
                filename="book.epub",
                payload=payload,
            )
        )
        self.assertEqual(message["To"], "kindle_123@kindle.com")
        attachment = next(message.iter_attachments())
        self.assertEqual(attachment.get_filename(), "book.epub")

    def test_rejects_non_epub_payload(self):
        with self.assertRaises(ValueError):
            validate_epub(b"not an epub")

    def test_rejects_compressed_mimetype_entry(self):
        output = BytesIO()
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("mimetype", "application/epub+zip")

        with self.assertRaises(ValueError):
            validate_epub(output.getvalue())


if __name__ == "__main__":
    unittest.main()
