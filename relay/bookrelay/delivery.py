import io
import smtplib
import zipfile
from email.message import EmailMessage


MAX_EPUB_BYTES = 200 * 1024 * 1024


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


def build_epub_email(sender: str, recipient: str, filename: str, payload: bytes) -> bytes:
    validate_epub(payload)
    message = EmailMessage()
    message["From"] = sender
    message["To"] = recipient
    message["Subject"] = "BookRelay delivery"
    message.set_content("BookRelay attached your EPUB. Kindle will process it through Send to Kindle.")
    message.add_attachment(payload, maintype="application", subtype="epub+zip", filename=filename)
    return message.as_bytes()


class SmtpMailer:
    def __init__(self, host: str, port: int, username: str, password: str, sender: str, use_tls: bool = True):
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.sender = sender
        self.use_tls = use_tls

    def send(self, recipient: str, filename: str, payload: bytes):
        raw = build_epub_email(self.sender, recipient, filename, payload)
        with smtplib.SMTP(self.host, self.port, timeout=30) as smtp:
            if self.use_tls:
                smtp.starttls()
            if self.username:
                smtp.login(self.username, self.password)
            smtp.sendmail(self.sender, [recipient], raw)
