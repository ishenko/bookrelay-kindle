# Kindle setup and rollback

This guide assumes a jailbroken Kindle Paperwhite 11 or 12 running firmware 5.19.0 or newer. The release artifact must be the kindlehf build.

## Amazon setup

Create a dedicated SMTP sender address for the relay. Add that address to Amazon's Approved Personal Document E-mail List. Find the Kindle's Send to Kindle e-mail address in the Amazon device settings and use that address during pairing.

Set the following values in Dokploy before pairing:

    BOOKRELAY_PAIRING_ADMIN_KEY=<long random owner key>
    BOOKRELAY_DEFAULT_KINDLE_EMAIL=<your Kindle Send to Kindle address>
    BOOKRELAY_SMTP_*=<SMTP settings>
    BOOKRELAY_DELIVERY_ENABLED=true

The relay sends EPUB as an email attachment. Amazon's processing and delivery time is outside the application's control; the client reports that the relay accepted or sent the job.

## Install

### Online installation from GitHub

Run these commands in the Kindle terminal or KPM shell:

    kpm add-repo https://raw.githubusercontent.com/ishenko/bookrelay-kindle/main/kpm/manifest.json
    kpm update
    kpm install bookrelay-kindle

For a later version, run kpm install bookrelay-kindle again. KPM selects the
kindlehf package for Paperwhite 11 and 12.

### Offline USB installation

Copy the complete bookrelay-kpm folder to the Kindle USB root. The expected
paths are /mnt/us/bookrelay-kpm/manifest.json and
/mnt/us/bookrelay-kpm/packages/bookrelay-kindle_0.1.0_kindlehf.kpkg.
Then run:

    kpm add-repo file:///mnt/us/bookrelay-kpm/manifest.json
    kpm update
    kpm install bookrelay-kindle

Do not copy the .kpkg directly to /mnt/us/kmc/kpm/packages/; KPM needs the
repository manifest to index and install it.

After installation:

1. Launch BookRelay Kindle from the installed application entry.
2. On a computer, open https://your-relay.example/pair, enter the Dokploy owner key, and copy the generated one-time code.
3. Press Pairing on Kindle, enter the HTTPS relay URL and the code, then press Connect.
4. Search for a book and choose Send to Kindle.

The client stores its relay URL, the Kindle Email returned by the relay, and a revocable token below user storage. The one-time code is not stored. The token can be revoked from the relay API by calling the revoke action with the token, or by deleting the device record from the relay database during maintenance.

## Removal

Run the package's uninstall hook from the jailbreak package manager. It removes the launcher at /mnt/us/documents/bookrelay-kindle.sh; the package manager then removes the installed package directory. It does not touch rootfs, system databases, OTA configuration, or the bootloader.

If the client does not launch, remove the launcher file from user storage and restart the Kindle application launcher. No firmware recovery should be required because the package never writes system partitions.

## Troubleshooting

- If search returns 401, pair the device again or check whether its relay token was revoked.
- If the relay accepts a job but no book arrives, inspect SMTP logs and Amazon's approved sender list.
- If covers are missing, the book and metadata APIs can still work; cover retrieval is a separate HTTP request.
- If an EPUB is rejected, verify that it is a valid EPUB ZIP with an uncompressed mimetype entry and is below the relay size limit.
