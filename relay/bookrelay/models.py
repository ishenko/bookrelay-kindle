from dataclasses import asdict, dataclass
from typing import Optional


@dataclass(frozen=True)
class Book:
    id: str
    title: str
    author: str = ""
    cover_url: str = ""
    description: str = ""
    translator: str = ""
    year: Optional[int] = None

    def to_dict(self):
        return asdict(self)


@dataclass(frozen=True)
class Pairing:
    code: str
    device_id: str
    expires_at: str
    kindle_email: str
