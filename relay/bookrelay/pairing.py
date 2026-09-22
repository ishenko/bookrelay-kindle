import hashlib
import re
import secrets
import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path

from .models import Pairing


EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")


def utc_now():
    return datetime.now(timezone.utc)


def hash_token(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


class PairingStore:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS pairings (
                    code TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL,
                    expires_at TEXT NOT NULL,
                    claimed_at TEXT,
                    kindle_email TEXT,
                    token TEXT
                );
                CREATE TABLE IF NOT EXISTS devices (
                    token_hash TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL,
                    kindle_email TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    revoked_at TEXT
                );
                """
            )

    def _connect(self):
        conn = sqlite3.connect(self.path)
        conn.row_factory = sqlite3.Row
        return conn

    def start_pairing(self, device_id: str, ttl: timedelta = timedelta(minutes=10)) -> Pairing:
        code = secrets.token_hex(4).upper()
        expires_at = utc_now() + ttl
        with self._connect() as conn:
            conn.execute(
                "INSERT INTO pairings(code, device_id, expires_at) VALUES (?, ?, ?)",
                (code, device_id, expires_at.isoformat()),
            )
        return Pairing(code=code, device_id=device_id, expires_at=expires_at.isoformat())

    def claim(self, code: str, kindle_email: str):
        if not EMAIL_RE.match(kindle_email):
            raise ValueError("invalid Kindle Email")
        now = utc_now()
        token = secrets.token_urlsafe(32)
        with self._connect() as conn:
            row = conn.execute("SELECT * FROM pairings WHERE code = ?", (code.upper(),)).fetchone()
            if not row:
                raise ValueError("pairing code not found")
            if row["claimed_at"]:
                raise ValueError("pairing code already claimed")
            if datetime.fromisoformat(row["expires_at"]) <= now:
                raise ValueError("pairing code expired")
            conn.execute(
                "UPDATE pairings SET claimed_at = ?, kindle_email = ?, token = ? WHERE code = ?",
                (now.isoformat(), kindle_email, token, code.upper()),
            )
            conn.execute(
                "INSERT OR REPLACE INTO devices(token_hash, device_id, kindle_email, created_at) VALUES (?, ?, ?, ?)",
                (hash_token(token), row["device_id"], kindle_email, now.isoformat()),
            )
        return {"device_id": row["device_id"], "kindle_email": kindle_email, "token": token}

    def status(self, code: str):
        with self._connect() as conn:
            row = conn.execute("SELECT * FROM pairings WHERE code = ?", (code.upper(),)).fetchone()
        if not row:
            return {"status": "not_found"}
        if row["claimed_at"]:
            return {
                "status": "claimed",
                "device_id": row["device_id"],
                "kindle_email": row["kindle_email"],
                "token": row["token"],
            }
        if datetime.fromisoformat(row["expires_at"]) <= utc_now():
            return {"status": "expired"}
        return {"status": "pending", "expires_at": row["expires_at"]}

    def authenticate(self, token: str):
        with self._connect() as conn:
            row = conn.execute(
                "SELECT device_id, kindle_email FROM devices WHERE token_hash = ? AND revoked_at IS NULL",
                (hash_token(token),),
            ).fetchone()
        return dict(row) if row else None

    def revoke(self, token: str):
        with self._connect() as conn:
            conn.execute(
                "UPDATE devices SET revoked_at = ? WHERE token_hash = ?",
                (utc_now().isoformat(), hash_token(token)),
            )
