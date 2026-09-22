# Kindle setup and rollback

This guide assumes a jailbroken Kindle Paperwhite 11 or 12 running firmware 5.19.0 or newer. The release artifact must be the kindlehf build.

## Amazon setup

Create a dedicated SMTP sender address for the relay. Add that address to Amazon's Approved Personal Document E-mail List. Find the Kindle's Send to Kindle e-mail address in the Amazon device settings and use that address during pairing.

The relay sends EPUB as an email attachment. Amazon's processing and delivery time is outside the application's control; the client reports that the relay accepted or sent the job.

## Install

1. Build the client with the Kindle SDK.
2. Build the KPM archive with scripts/package-kpm.py.
3. Copy the archive to user storage.
4. Install it with the KPM package manager in the jailbreak environment.
5. Launch BookRelay Kindle from the installed application entry.
6. Open Settings, enter the HTTPS relay URL, and save.
7. Press Pairing and complete the one-time code form at the relay's /pair page.

The client stores its configuration below user storage. The token can be revoked from the relay API by calling the revoke action with the token, or by deleting the device record from the relay database during maintenance.

## Removal

Run the package's uninstall hook from the jailbreak package manager. It removes the launcher at /mnt/us/documents/bookrelay-kindle.sh; the package manager then removes the installed package directory. It does not touch rootfs, system databases, OTA configuration, or the bootloader.

If the client does not launch, remove the launcher file from user storage and restart the Kindle application launcher. No firmware recovery should be required because the package never writes system partitions.

## Troubleshooting

- If search returns 401, pair the device again or check whether its relay token was revoked.
- If the relay accepts a job but no book arrives, inspect SMTP logs and Amazon's approved sender list.
- If covers are missing, the book and metadata APIs can still work; cover retrieval is a separate HTTP request.
- If an EPUB is rejected, verify that it is a valid EPUB ZIP with an uncompressed mimetype entry and is below the relay size limit.
