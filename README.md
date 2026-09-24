# BookRelay Kindle

BookRelay Kindle is an AGPL-3.0 self-hosted relay and a native Kindle client for browsing a configured Flibusta source and sending EPUB books to a Kindle through Amazon Send to Kindle.

The first compatibility target is deliberately narrow:

| Device | Firmware | Build target |
| --- | --- | --- |
| Kindle Paperwhite 11 | 5.19.0 and newer | kindlehf |
| Kindle Paperwhite 12 | 5.19.0 and newer | kindlehf |
| Paperwhite Signature Edition | 5.19.0 and newer | kindlehf |

The client is packaged as a KPM-style user-storage application. It does not modify rootfs, remount system partitions, change OTA settings, edit Kindle system databases, install a bootloader, or require KOReader. Removing the package removes its installed package files and launcher from user storage; the separate settings/cache directory can be removed if a full local reset is desired. The EPUB arrives through Amazon and appears in the normal Kindle library after Amazon processes it.

## How it works

1. Run the relay on your own VPS with Docker Compose.
2. Add the relay SMTP sender to Amazon's Approved Personal Document E-mail List.
3. Open the relay pairing page on a computer, enter the Kindle Email for the device and the owner key, and copy the one-time code.
4. On the Kindle, enter the server hostname (without https://), Kindle Email, and one-time code on the same screen, then press Connect or Enter. The client updates the delivery email on the relay after claiming the code.
5. Search books on Kindle and choose Send to Kindle. Tap the star on a book page to save it locally; the star in the top bar opens favorites.

The relay stores only job metadata and temporary delivery data needed to send the EPUB. It does not bypass DRM, CAPTCHA, or source authentication.

## Relay deployment

Copy the example environment file, set SMTP credentials, and start the service:

    cp .env.example .env
    docker compose up -d --build

Set BOOKRELAY_PAIRING_ADMIN_KEY to a long random secret. BOOKRELAY_DEFAULT_KINDLE_EMAIL is optional and only serves as a fallback for older pairing links; the pairing page accepts a separate Kindle Email for every device. BOOKRELAY_DELIVERY_ENABLED defaults to true, and should be set to false while SMTP is being configured if you want the relay to fail closed. Enter the owner key only on the HTTPS pairing page. SMTP credentials stay on the relay. The Kindle Email is entered both when generating a code on /pair and on the Kindle; the Kindle value is saved to the relay after claim.

The relay listens on port 8000 by default. Put it behind HTTPS before using it from a Kindle. A reverse proxy such as Caddy or nginx should terminate TLS and limit access to the pairing page and API as appropriate.

The standard Flibusta relay image includes a snapshot of 24 categories and 271 subcategories, so navigation does not wait for Flibusta. Book lists, search, covers, and EPUB files still come from the live source. Update the snapshot when the source changes its categories.

SQLite is the default database. The container stores it in the relay-data volume. A PostgreSQL adapter is intentionally left for a later scale-out change.

## Kindle installation

The safest online installation uses KTerm. First, on the Kindle home screen,
open the global search field and run these commands with a leading semicolon:

    ;kpm install kterm
    ;kpm launch kterm

Then enter the following commands inside KTerm (without the leading semicolon):

    kpm add-repo https://book.fffq.uk/i
    kpm update
    kpm install bookrelay-kindle

The Kindle search command handler may reject `:` and dots in a URL before KPM
starts, so the repository URL should be entered in KTerm.

After a new GitHub Release, update the installed package with:

    kpm install bookrelay-kindle

To prepare the package offline, copy the whole bookrelay-kpm folder from the
release files to the Kindle USB root. It must contain manifest.json and
packages/bookrelay-kindle_0.1.23_kindlehf.kpkg. Then run:

    kpm add-repo file:///mnt/us/bookrelay-kpm/manifest.json
    kpm update
    kpm install bookrelay-kindle

For maintainers, build the client with the Kindle SDK and its kindlehf cross file:

    meson setup client/build client --cross-file /path/to/kindlehf.ini
    meson compile -C client/build
    python scripts/package-kpm.py --binary client/build/bookrelay-kindle --output dist/bookrelay-kindle.kpkg

Copy the resulting package to Kindle user storage and install it using the KPM package manager available in the jailbreak environment. The package contains only a user-storage application and hooks. Follow the removal instructions in docs/KINDLE-SETUP.md if you need to roll back.

This repository does not claim a local macOS build is a Kindle SDK build. CI runs both a host Linux integration build and a real kindlehf cross-build; release artifacts are produced from the latter.

On Kindle, text fields request the device's own on-screen keyboard. If it does
not appear, tap **Клавиатура BookRelay** above the keyboard area to use the
built-in fallback, which includes an HTTPS prefix key and a symbols layout.
On desktop or if Kindle's keyboard service is unavailable, the fallback opens
automatically. Set BOOKRELAY_KEYBOARD=custom to use it from the start.
The system keyboard integration still needs a physical-device input check.

## Development

### Browser preview

Before installing the KPM package, open the clickable browser preview of the Kindle UI. It uses local demo fixtures and mock relay responses; it does not contact Flibusta, Amazon, SMTP, or a Kindle device.

    cd preview
    python3 -m http.server 4173

Open http://127.0.0.1:4173 in a browser. The preview covers search, categories, page navigation, book details, pairing, settings, and the simulated delivery status flow. It is a visual interaction check, not a Kindle SDK build or an emulator of the native GTK binary.

Run the relay tests locally:

    PYTHONPATH=relay python -m unittest discover -s relay/tests -v
    python -m py_compile relay/bookrelay/*.py relay/bookrelay/source/*.py
    python scripts/check-kpm.py

See docs/DEVELOPMENT.md for the client toolchain and release checklist. See docs/SECURITY.md for the threat model and rollback guarantees.

## License

BookRelay Kindle is licensed under AGPL-3.0. See LICENSE.
