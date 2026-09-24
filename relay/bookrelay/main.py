import json
import hashlib
import hmac
import os
import re
import secrets
from pathlib import Path
from urllib.parse import quote

from collections import deque
from threading import Lock
from time import monotonic

from fastapi import BackgroundTasks, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import HTMLResponse, JSONResponse, Response
from pydantic import BaseModel, ConfigDict, Field

from .delivery import SmtpMailer
from .jobs import DeliveryService, JobStore
from .pairing import PairingStore, utc_now
from .source.flibusta import FlibustaSource, SourceUnavailable


class PairStartRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    device_id: str = Field(min_length=1, max_length=128)
    kindle_email: str | None = Field(default=None, max_length=320)
    admin_key: str = Field(default="", max_length=256)


class PairClaimRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    code: str = Field(min_length=4, max_length=32)


class DeviceEmailRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    kindle_email: str = Field(min_length=3, max_length=320)


class DeliveryRequest(BaseModel):
    book_id: str = Field(min_length=1, max_length=128)
    title: str | None = Field(default=None, max_length=300)


PAIRING_PAGE = """<!doctype html>
<html lang='ru'>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>BookRelay — подключение Kindle</title>
<style>
  body { font: 16px/1.5 system-ui, sans-serif; max-width: 34rem; margin: 3rem auto; padding: 0 1rem; color: #202020; }
  h1 { line-height: 1.2; }
  label { display: block; margin: 1rem 0 .3rem; font-weight: 600; }
  input, button { font: inherit; padding: .75rem; width: 100%; box-sizing: border-box; }
  input { border: 1px solid #999; border-radius: 4px; }
  button { margin-top: 1.5rem; background: #202020; color: white; border: 0; border-radius: 4px; cursor: pointer; }
  button:disabled { opacity: .5; cursor: wait; }
  #result { margin-top: 2rem; padding: 1.5rem; border: 1px solid #aaa; border-radius: 4px; }
  #code { display: block; font: 700 2rem ui-monospace, monospace; letter-spacing: .12em; overflow-wrap: anywhere; }
  progress { width: 100%; height: 1rem; accent-color: #202020; }
  #error { color: #a22; }
</style>
<h1>Подключение Kindle</h1>
<p>Укажите почту Kindle и ключ владельца сервера. Одноразовый код действует 10 минут.</p>
<form id='pair'>
  <label for='kindle-email'>Почта Kindle</label>
  <input id='kindle-email' name='kindle_email' type='email' required autocomplete='email' placeholder='your_device@kindle.com'>
  <label for='admin-key'>Ключ владельца (Dokploy Environment)</label>
  <input id='admin-key' name='admin_key' type='password' required autocomplete='off'>
  <button type='submit'>Получить код</button>
</form>
<p id='error' role='alert' hidden></p>
<section id='result' aria-live='polite' hidden>
  <p>Введите этот код на Kindle:</p>
  <strong id='code'></strong>
  <p id='remaining'></p>
  <progress id='lifetime' max='1' value='1' aria-label='Оставшееся время'></progress>
  <p id='expires'></p>
</section>
<script>
  const form = document.querySelector('#pair');
  const result = document.querySelector('#result');
  const error = document.querySelector('#error');
  const remaining = document.querySelector('#remaining');
  const progress = document.querySelector('#lifetime');
  let timer;

  form.addEventListener('submit', async event => {
    event.preventDefault();
    clearInterval(timer);
    result.hidden = true;
    error.hidden = true;
    const button = form.querySelector('button');
    button.disabled = true;
    const fields = new FormData(form);
    const body = {
      device_id: 'web-' + (crypto.randomUUID ? crypto.randomUUID() : Date.now()),
      kindle_email: fields.get('kindle_email'),
      admin_key: fields.get('admin_key'),
    };
    try {
      const response = await fetch('/v1/pair/start', {
        method: 'POST',
        headers: {'content-type': 'application/json'},
        body: JSON.stringify(body),
      });
      const data = await response.json();
      if (!response.ok) throw new Error(data.detail || 'Не удалось получить код');
      const lifetimeMs = Date.parse(data.expires_at) - Date.parse(data.server_time);
      if (!Number.isFinite(lifetimeMs) || lifetimeMs <= 0) throw new Error('Сервер вернул неверное время действия кода');
      const started = performance.now();
      result.querySelector('#code').textContent = data.code;
      result.querySelector('#expires').textContent = 'Истекает: ' + new Date(data.expires_at).toLocaleString();
      result.hidden = false;
      const tick = () => {
        const seconds = Math.max(0, Math.ceil((lifetimeMs - (performance.now() - started)) / 1000));
        remaining.textContent = seconds ? 'Осталось ' + Math.floor(seconds / 60) + ':' + String(seconds % 60).padStart(2, '0') : 'Срок действия кода истёк';
        progress.value = Math.max(0, Math.min(1, (lifetimeMs - (performance.now() - started)) / lifetimeMs));
        if (!seconds) clearInterval(timer);
      };
      tick();
      timer = setInterval(tick, 1000);
    } catch (failure) {
      error.textContent = failure.message || 'Не удалось получить код';
      error.hidden = false;
    } finally {
      button.disabled = false;
    }
  });
</script>
</html>"""
KPM_MANIFEST_PATH = Path(__file__).resolve().parents[2] / "kpm" / "manifest.json"


def _token(authorization: str | None):
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(status_code=401, detail="Bearer token required")
    return authorization[7:].strip()


class RateLimiter:
    def __init__(self):
        self.events = {}
        self.expires = {}
        self.lock = Lock()
        self.calls = 0

    def allow(self, key: str, limit: int, window: float) -> bool:
        now = monotonic()
        with self.lock:
            self.calls += 1
            if self.calls % 256 == 0:
                # Public endpoints can see a new address on every request.
                # Discard expired address buckets instead of retaining them forever.
                for address, expires_at in list(self.expires.items()):
                    if expires_at <= now:
                        del self.events[address]
                        del self.expires[address]
            bucket = self.events.setdefault(key, deque())
            while bucket and bucket[0] <= now - window:
                bucket.popleft()
            if len(bucket) >= limit:
                return False
            bucket.append(now)
            self.expires[key] = now + window
            return True


def create_app(db_path: Path | str | None = None, source=None, mailer=None, pairing_admin_key: str | None = None, default_kindle_email: str | None = None, delivery_enabled: bool | None = None) -> FastAPI:
    admin_key = pairing_admin_key if pairing_admin_key is not None else os.getenv("BOOKRELAY_PAIRING_ADMIN_KEY", "")
    default_email = default_kindle_email if default_kindle_email is not None else os.getenv("BOOKRELAY_DEFAULT_KINDLE_EMAIL", "")
    if delivery_enabled is None:
        delivery_enabled = os.getenv("BOOKRELAY_DELIVERY_ENABLED", "true").lower() == "true"
    root = Path(db_path or os.getenv("BOOKRELAY_DB", "./data/relay.sqlite3"))
    source_url = os.getenv("BOOKRELAY_SOURCE_URL", "https://flibusta.is")
    source = source or FlibustaSource(
        source_url,
        snapshot_path=Path(__file__).resolve().parents[2] / "client/share/subcategories.json"
        if source_url.rstrip("/") == "https://flibusta.is" else None,
    )
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
    app = FastAPI(title="BookRelay Relay", version="0.1.7")
    app.state.pairing = pairing
    app.state.source = source
    app.state.delivery = delivery

    @app.exception_handler(SourceUnavailable)
    async def source_unavailable(request: Request, exc: SourceUnavailable):
        return JSONResponse(status_code=503, content={"detail": str(exc)})
    app.state.limiter = limiter
    app.state.default_kindle_email = default_email
    # Archive image ids do not encode the book id. Authenticate the exact
    # book/path pair returned by OPDS rather than accepting arbitrary /ib URLs.
    cover_key = (hashlib.sha256(b"bookrelay-cover-v1\0" + admin_key.encode()).digest()
                 if admin_key else secrets.token_bytes(32))

    def cover_signature(book_id: str, path: str) -> str:
        return hmac.new(cover_key, f"{book_id}\0{path}".encode(), hashlib.sha256).hexdigest()

    def require_device(authorization: str | None):
        device = pairing.authenticate(_token(authorization))
        if not device:
            raise HTTPException(status_code=401, detail="invalid or revoked token")
        return device

    def limit(request: Request, scope: str, count: int, window: float):
        address = request.client.host if request.client else "unknown"
        if not limiter.allow(f"{scope}:{address}", count, window):
            raise HTTPException(status_code=429, detail="rate limit exceeded")

    def serialize_book(book):
        data = book.to_dict()
        if isinstance(source, FlibustaSource) and book.cover_url:
            try:
                path = source.cover_path(book.id, book.cover_url)
            except ValueError:
                data["cover_url"] = ""
            else:
                url = f"/v1/books/{book.id}/cover?path={quote(path, safe='')}"
                if path.startswith("/ib/"):
                    url += f"&sig={cover_signature(book.id, path)}"
                data["cover_url"] = url
        return data

    @app.get("/healthz")
    def healthz():
        return {"status": "ok", "delivery_enabled": delivery_enabled, "pairing_enabled": bool(admin_key)}

    @app.get("/pair", response_class=HTMLResponse)
    def pairing_page():
        return PAIRING_PAGE

    @app.get("/i", include_in_schema=False)
    def install_manifest():
        try:
            manifest = json.loads(KPM_MANIFEST_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise HTTPException(status_code=503, detail="KPM manifest is unavailable") from exc
        return JSONResponse(content=manifest, headers={"Cache-Control": "public, max-age=300"})

    @app.get("/v1/categories")
    def categories(request: Request, authorization: str | None = Header(default=None)):
        limit(request, "categories", 30, 60)
        require_device(authorization)
        return source.categories()

    @app.get("/v1/subcategories")
    def subcategories(request: Request, category: str = Query(max_length=500), authorization: str | None = Header(default=None)):
        limit(request, "subcategories", 60, 60)
        require_device(authorization)
        try:
            return source.subcategories(category)
        except ValueError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc

    @app.get("/v1/catalog/books")
    def catalog_books(request: Request, category: str = Query(max_length=500), subcategory: str = Query(max_length=500), page: int = Query(default=1, ge=1, le=100), size: int = Query(default=6, ge=4, le=12), authorization: str | None = Header(default=None)):
        limit(request, "catalog-books", 60, 60)
        require_device(authorization)
        try:
            books, has_next = source.catalog_books(category, subcategory, page, size)
        except ValueError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc
        return {"items": [serialize_book(book) for book in books], "page": page, "has_next": has_next}

    @app.get("/v1/search")
    def search(request: Request, q: str = Query(default="", max_length=200), category: str | None = Query(default=None, max_length=64), page: int = Query(default=1, ge=1, le=100), size: int = Query(default=6, ge=4, le=12), authorization: str | None = Header(default=None)):
        limit(request, "search", 60, 60)
        require_device(authorization)
        if not q.strip() and not category:
            raise HTTPException(status_code=400, detail="query or category is required")
        if hasattr(source, "search_page") and not category:
            books, has_next = source.search_page(q, page, size)
        else:
            books, has_next = source.search(q, page, category), False
        return {"items": [serialize_book(book) for book in books], "page": page, "query": q, "category": category, "has_next": has_next}

    @app.get("/v1/books/{book_id}/cover")
    def book_cover(request: Request, book_id: str, path: str = Query(max_length=256), sig: str = Query(default="", max_length=64), authorization: str | None = Header(default=None)):
        limit(request, "book-cover", 120, 60)
        require_device(authorization)
        if not isinstance(source, FlibustaSource):
            raise HTTPException(status_code=404, detail="cover unavailable")
        if path.startswith("/ib/") and (not re.fullmatch(r"[0-9a-f]{64}", sig) or
                                          not hmac.compare_digest(sig, cover_signature(book_id, path))):
            raise HTTPException(status_code=404, detail="cover unavailable")
        try:
            payload = source.download_cover(book_id, path)
        except ValueError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc
        return Response(content=payload, media_type="image/png" if payload.startswith(b"\x89PNG\r\n\x1a\n") else "image/jpeg",
                        headers={"Cache-Control": "private, max-age=86400"})

    @app.get("/v1/books/{book_id}")
    def book_details(book_id: str, authorization: str | None = Header(default=None)):
        require_device(authorization)
        return serialize_book(source.details(book_id))

    @app.post("/v1/pair/start")
    def pair_start(request: Request, payload: PairStartRequest):
        limit(request, "pair-start", 10, 600)
        if not admin_key:
            raise HTTPException(status_code=503, detail="pairing is not configured")
        if not secrets.compare_digest(payload.admin_key.encode(), admin_key.encode()):
            raise HTTPException(status_code=403, detail="owner key required")
        target_email = payload.kindle_email or default_email
        if not target_email:
            raise HTTPException(status_code=400, detail="Kindle Email is required")
        try:
            result = pairing.start_pairing(payload.device_id, target_email)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        return {"code": result.code, "expires_at": result.expires_at, "server_time": utc_now().isoformat(), "pairing_url": "/pair"}

    @app.post("/v1/pair/claim")
    def pair_claim(request: Request, payload: PairClaimRequest):
        limit(request, "pair-claim", 10, 600)
        try:
            return pairing.claim(payload.code)
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

    @app.put("/v1/devices/me")
    def update_device_email(request: Request, payload: DeviceEmailRequest, authorization: str | None = Header(default=None)):
        limit(request, "device-update", 10, 600)
        token = _token(authorization)
        try:
            email = pairing.update_kindle_email(token, payload.kindle_email)
        except ValueError as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc
        except PermissionError as exc:
            raise HTTPException(status_code=401, detail=str(exc)) from exc
        return {"kindle_email": email}

    return app


app = create_app()
