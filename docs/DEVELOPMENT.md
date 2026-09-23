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

The tested SDK setup uses the community Kindle SDK and the matching kindlehf toolchain. Keep both outside the repository:

    git clone --recurse-submodules https://github.com/KindleModding/kindle-sdk.git kindle-sdk
    wget https://github.com/KindleModding/koxtoolchain/releases/latest/download/kindlehf.tar.gz -O /tmp/kindlehf.tar.gz
    tar -xzf /tmp/kindlehf.tar.gz -C "$HOME"
    sudo bash kindle-sdk/gen-sdk.sh kindlehf "$HOME/x-tools/arm-kindlehf-linux-gnueabihf"

Then build with the generated cross file:

    meson setup client/build-kindlehf client --cross-file "$HOME/x-tools/arm-kindlehf-linux-gnueabihf/meson-crosscompile.txt"
    meson compile -C client/build-kindlehf
    file client/build-kindlehf/bookrelay-kindle
    readelf -h client/build-kindlehf/bookrelay-kindle

Do not copy a host Linux binary into a Kindle package. The package manifest declares kindlehf and firmware 5.19.0 as the minimum compatibility target.

## Release checks

    python scripts/check-kpm.py
    python scripts/test_check_kpm_repository.py
    python scripts/check-kpm-repository.py --release-tag v0.1.19
    python scripts/package-kpm.py --binary client/build/bookrelay-kindle --output dist/bookrelay-kindle.kpkg
    tar -tzf dist/bookrelay-kindle.kpkg
    git diff --check

Pushing a vMAJOR.MINOR.PATCH tag runs .github/workflows/release.yml. The
workflow performs the real kindlehf cross-build, packages the KPM archive and
publishes the archive plus its SHA-256 checksum as a GitHub Release asset.

The repository and package manifests use KPM manifest format version 2. The
package manifest must not use a newer version because older KPM clients reject
it before inspecting the package contents.

Before testing on hardware, verify the install and uninstall hooks in a disposable user-storage directory. On hardware, check launch, search, pairing, cover failure, delivery acceptance and uninstall. Do not test by modifying rootfs or remounting system partitions.
