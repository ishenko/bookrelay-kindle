import io
import smtplib
import zipfile
from email.message import EmailMessage


MAX_EPUB_BYTES = 200 * 1024 * 1024
MAX_PDF_BYTES = MAX_EPUB_BYTES


def validate_epub(payload: bytes, max_bytes: int = MAX_EPUB_BYTES):
    if not payload or len(payload) > max_bytes:
        raise ValueError("EPUB is empty or exceeds the configured size limit")
    try:
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            mimetype = archive.getinfo("mimetype")
            if mimetype.compress_type != zipfile.ZIP_STORED or archive.read("mimetype") != b"application/epub+zip":
                raise ValueError("EPUB mimetype is missing or invalid")
            if sum(info.file_size for info in archive.infolist()) > 500 * 1024 * 1024:
                raise ValueError("EPUB expands beyond the safety limit")
    except (KeyError, zipfile.BadZipFile) as exc:
        raise ValueError("payload is not a valid EPUB") from exc


def detect_book_format(payload: bytes) -> str:
    if payload.startswith(b"%PDF-"):
        if (len(payload) > MAX_PDF_BYTES or len(payload) < 8 or
                b"%%EOF" not in payload[-1024:]):
            raise ValueError("PDF is incomplete or exceeds the configured size limit")
        return "pdf"
    validate_epub(payload)
    return "epub"


def build_book_email(sender: str, recipient: str, filename: str, payload: bytes) -> bytes:
    book_format = detect_book_format(payload)
    if not filename.lower().endswith("." + book_format):
        raise ValueError("attachment filename does not match the book format")
    message = EmailMessage()
    message["From"] = sender
    message["To"] = recipient
    message["Subject"] = "BookRelay delivery"
    message.set_content(f"BookRelay attached your {book_format.upper()}. Kindle will process it through Send to Kindle.")
    message.add_attachment(payload, maintype="application",
                           subtype="pdf" if book_format == "pdf" else "epub+zip", filename=filename)
    return message.as_bytes()


def build_epub_email(sender: str, recipient: str, filename: str, payload: bytes) -> bytes:
    validate_epub(payload)
    return build_book_email(sender, recipient, filename, payload)


class SmtpMailer:
    def __init__(self, host: str, port: int, username: str, password: str, sender: str, use_tls: bool = True):
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.sender = sender
        self.use_tls = use_tls

    def send(self, recipient: str, filename: str, payload: bytes):
        raw = build_book_email(self.sender, recipient, filename, payload)
        with smtplib.SMTP(self.host, self.port, timeout=30) as smtp:
            if self.use_tls:
                smtp.starttls()
            if self.username:
                smtp.login(self.username, self.password)
            smtp.sendmail(self.sender, [recipient], raw)
