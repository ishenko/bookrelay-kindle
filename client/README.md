# Kindle client

The client is a small GTK application intended for the kindlehf target used by Kindle Paperwhite 11 and 12 on firmware 5.19.0 and newer.

It stores configuration in the user data directory and communicates only with the configured relay. It does not write Kindle system files, alter OTA settings, or place books into the Kindle database. Amazon delivery puts the EPUB into the normal Kindle library.

Build with the Kindle SDK toolchain and Meson. The exact cross file is supplied by the Kindle SDK:

    meson setup build --cross-file /path/to/kindlehf.ini
    meson compile -C build
