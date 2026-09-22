# Development and release checklist

## Relay

Use Python 3.11 or newer:

    python -m venv .venv
    . .venv/bin/activate
    pip install -r relay/requirements.txt httpx
    PYTHONPATH=relay python -m unittest discover -s relay/tests -v
    python -m py_compile relay/bookrelay/*.py relay/bookrelay/source/*.py

The source adapter is fixture-tested. Add fixtures before changing selectors because the upstream HTML can change.

## Kindle client

The client uses C11, GTK, GLib, GdkPixbuf and libcurl. A normal Linux build verifies the source and dependency wiring. A Kindle release must use the Kindle SDK cross compiler and the kindlehf sysroot.

    meson setup client/build client --cross-file /path/to/kindlehf.ini
    meson compile -C client/build

Do not copy a host Linux binary into a Kindle package. The package manifest declares kindlehf and firmware 5.19.0 as the minimum compatibility target.

## Release checks

    python scripts/check-kpm.py
    python scripts/package-kpm.py --binary client/build/bookrelay-kindle --output dist/bookrelay-kindle.kpkg
    tar -tzf dist/bookrelay-kindle.kpkg
    git diff --check

Before testing on hardware, verify the install and uninstall hooks in a disposable user-storage directory. On hardware, check launch, search, pairing, cover failure, delivery acceptance and uninstall. Do not test by modifying rootfs or remounting system partitions.
