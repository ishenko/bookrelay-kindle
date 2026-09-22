import re
import sqlite3
import uuid
from datetime import datetime, timezone
from pathlib import Path

from .delivery import validate_epub


def now_iso():
    return datetime.now(timezone.utc).isoformat()


def safe_filename(title: str) -> str:
    value = re.sub(r"[^A-Za-z0-9А-Яа-я _.-]+", "", title).strip(" .") or "book"
    return f"{value.replace(' ', '_')}.epub"


class JobStore:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as conn:
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS jobs (
                    id TEXT PRIMARY KEY,
                    book_id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    kindle_email TEXT NOT NULL,
                    status TEXT NOT NULL,
                    error TEXT,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                )
                """
            )

    def _connect(self):
        conn = sqlite3.connect(self.path)
        conn.row_factory = sqlite3.Row
        return conn

    def create(self, book_id: str, kindle_email: str, title: str):
        job_id = str(uuid.uuid4())
        timestamp = now_iso()
        with self._connect() as conn:
            conn.execute(
                "INSERT INTO jobs(id, book_id, title, kindle_email, status, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                (job_id, book_id, title, kindle_email, "queued", timestamp, timestamp),
            )
        return self.get(job_id)

    def update(self, job_id: str, status: str, error: str | None = None):
        with self._connect() as conn:
            conn.execute(
                "UPDATE jobs SET status = ?, error = ?, updated_at = ? WHERE id = ?",
                (status, error, now_iso(), job_id),
            )

    def get(self, job_id: str):
        with self._connect() as conn:
            row = conn.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone()
        return dict(row) if row else None


class DeliveryService:
    def __init__(self, jobs: JobStore, source, mailer):
        self.jobs = jobs
        self.source = source
        self.mailer = mailer

    def enqueue(self, book_id: str, kindle_email: str, title: str):
        return self.jobs.create(book_id, kindle_email, title)

    def run(self, job_id: str):
        job = self.jobs.get(job_id)
        if not job:
            return
        try:
            self.jobs.update(job_id, "downloading")
            payload = self.source.download(job["book_id"])
            validate_epub(payload)
            self.jobs.update(job_id, "sending")
            self.mailer.send(job["kindle_email"], safe_filename(job["title"]), payload)
            self.jobs.update(job_id, "sent")
        except Exception as exc:
            self.jobs.update(job_id, "failed", str(exc))
