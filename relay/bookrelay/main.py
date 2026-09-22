import os
import secrets
from pathlib import Path

from collections import defaultdict, deque
from time import monotonic

from fastapi import BackgroundTasks, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import HTMLResponse
from pydantic import BaseModel, Field

from .delivery import SmtpMailer
from .jobs import DeliveryService, JobStore
from .pairing import PairingStore
from .source.flibusta import FlibustaSource


class PairStartRequest(BaseModel):
    device_id: str = Field(min_length=1, max_length=128)


class PairClaimRequest(BaseModel):
    code: str = Field(min_length=4, max_length=32)
    kindle_email: str = Field(min_length=5, max_length=254)
    admin_key: str = Field(default="", max_length=256)


class DeliveryRequest(BaseModel):
    book_id: str = Field(min_length=1, max_length=128)
    title: str | None = Field(default=None, max_length=300)


PAIRING_PAGE = """<!doctype html>
<html lang='en'><meta charset='utf-8'><title>BookRelay pairing</title>
<style>body{font:16px system-ui;max-width:34rem;margin:3rem auto;padding:0 1rem}label{display:block;margin:.8rem 0 .25rem}input,button{font:inherit;padding:.6rem;width:100%;box-sizing:border-box}button{margin-top:1rem}</style>
<h1>BookRelay pairing</h1>
<p>Enter the one-time code shown on your Kindle and its Send to Kindle email address.</p>
<form id='pair'><label>Pairing code<input name='code' required autocomplete='off'></label><label>Kindle email<input name='kindle_email' type='email' required></label><label>Owner key (Dokploy Environment)<input name='admin_key' type='password' required autocomplete='off'></label><button>Pair device</button></form><p id='result'></p>
<script>document.querySelector('#pair').addEventListener('submit',async e=>{e.preventDefault();const body=Object.fromEntries(new FormData(e.target));const r=await fetch('/v1/pair/claim',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});document.querySelector('#result').textContent=r.ok?'Device paired. Return to Kindle.':(await r.json()).detail||'Pairing failed';});</script>
</html>"""


def _token(authorization: str | None):
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(status_code=401, detail="Bearer token required")
    return authorization[7:].strip()


class RateLimiter:
    def __init__(self):
        self.events = defaultdict(deque)

    def allow(self, key: str, limit: int, window: float) -> bool:
        now = monotonic()
        bucket = self.events[key]
        while bucket and bucket[0] <= now - window:
            bucket.popleft()
        if len(bucket) >= limit:
            return False
        bucket.append(now)
        return True


def create_app(db_path: Path | str | None = None, source=None, mailer=None, pairing_admin_key: str | None = None, delivery_enabled: bool | None = None) -> FastAPI:
    admin_key = pairing_admin_key if pairing_admin_key is not None else os.getenv("BOOKRELAY_PAIRING_ADMIN_KEY", "")
    if delivery_enabled is None:
        delivery_enabled = os.getenv("BOOKRELAY_DELIVERY_ENABLED", "false").lower() == "true"
    root = Path(db_path or os.getenv("BOOKRELAY_DB", "./data/relay.sqlite3"))
    source = source or FlibustaSource(os.getenv("BOOKRELAY_SOURCE_URL", "https://flibusta.is"))
    if mailer is None:
        mailer = SmtpMailer(
            host=os.getenv("BOOKRELAY_SMTP_HOST", "localhost"),
            port=int(os.getenv("BOOKRELAY_SMTP_PORT", "587")),
            username=os.getenv("BOOKRELAY_SMTP_USERNAME", ""),
            password=os.getenv("BOOKRELAY_SMTP_PASSWORD", ""),
            sender=os.getenv("BOOKRELAY_SMTP_FROM", "bookrelay@example.com"),
            use_tls=os.getenv("BOOKRELAY_SMTP_TLS", "true").lower() == "true",
        )
    pairing = PairingStore(root)
    jobs = JobStore(root)
    delivery = DeliveryService(jobs, source, mailer)
    limiter = RateLimiter()
    app = FastAPI(title="BookRelay Relay", version="0.1.0")
    app.state.pairing = pairing
    app.state.source = source
    app.state.delivery = delivery
    app.state.limiter = limiter

    def require_device(authorization: str | None):
        device = pairing.authenticate(_token(authorization))
        if not device:
            raise HTTPException(status_code=401, detail="invalid or revoked token")
        return device

    def limit(request: Request, scope: str, count: int, window: float):
        address = request.client.host if request.client else "unknown"
        if not limiter.allow(f"{scope}:{address}", count, window):
            raise HTTPException(status_code=429, detail="rate limit exceeded")

    @app.get("/healthz")
    def healthz():
        return {"status": "ok", "delivery_enabled": delivery_enabled, "pairing_enabled": bool(admin_key)}

    @app.get("/pair", response_class=HTMLResponse)
    def pairing_page():
        return PAIRING_PAGE

    @app.get("/v1/categories")
    def categories(request: Request, authorization: str | None = Header(default=None)):
        limit(request, "categories", 30, 60)
        require_device(authorization)
        return source.categories()

    @app.get("/v1/search")
    def search(request: Request, q: str = Query(min_length=1, max_length=200), page: int = Query(default=1, ge=1, le=100), authorization: str | None = Header(default=None)):
        limit(request, "search", 60, 60)
        require_device(authorization)
        return {"items": [book.to_dict() for book in source.search(q, page)], "page": page, "query": q}

    @app.get("/v1/books/{book_id}")
    def book_details(book_id: str, authorization: str | None = Header(default=None)):
        require_device(authorization)
        return source.details(book_id).to_dict()

    @app.post("/v1/pair/start")
    def pair_start(request: Request, payload: PairStartRequest):
        limit(request, "pair-start", 10, 600)
        result = pairing.start_pairing(payload.device_id)
        return {"code": result.code, "expires_at": result.expires_at, "pairing_url": "/pair"}

    @app.post("/v1/pair/claim")
    def pair_claim(request: Request, payload: PairClaimRequest):
        limit(request, "pair-claim", 10, 600)
        if not admin_key:
            raise HTTPException(status_code=503, detail="pairing is not configured")
        if not secrets.compare_digest(payload.admin_key.encode(), admin_key.encode()):
            raise HTTPException(status_code=403, detail="owner key required")
        try:
            return pairing.claim(payload.code, payload.kindle_email)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

    @app.get("/v1/pair/status/{code}")
    def pair_status(request: Request, code: str):
        limit(request, "pair-status", 60, 600)
        result = pairing.status(code)
        if result["status"] == "not_found":
            raise HTTPException(status_code=404, detail="pairing code not found")
        return result

    @app.post("/v1/deliveries", status_code=202)
    def create_delivery(request: Request, payload: DeliveryRequest, background: BackgroundTasks, authorization: str | None = Header(default=None)):
        limit(request, "delivery", 20, 3600)
        device = require_device(authorization)
        if not delivery_enabled:
            raise HTTPException(status_code=503, detail="delivery is disabled until SMTP is configured")
        title = payload.title or source.details(payload.book_id).title
        job = delivery.enqueue(payload.book_id, device["kindle_email"], title)
        background.add_task(delivery.run, job["id"])
        return {"id": job["id"], "status": job["status"], "message": "Book is being sent to Kindle"}

    @app.get("/v1/deliveries/{job_id}")
    def get_delivery(job_id: str, authorization: str | None = Header(default=None)):
        device = require_device(authorization)
        job = jobs.get(job_id)
        if not job or job["kindle_email"] != device["kindle_email"]:
            raise HTTPException(status_code=404, detail="delivery not found")
        return job

    @app.post("/v1/devices/revoke")
    def revoke_device(authorization: str | None = Header(default=None)):
        pairing.revoke(_token(authorization))
        return {"status": "revoked"}

    return app


app = create_app()
