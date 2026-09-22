# Security model

BookRelay is designed to keep the Kindle side reversible and the relay private.

- Pairing uses a short-lived one-time code.
- The relay returns a revocable bearer token only after the Kindle claims a one-time code.
- Catalog and delivery endpoints require a valid token.
- Search, pairing and delivery requests have in-process rate limits.
- EPUB size and ZIP expansion limits are checked before sending mail.
- SMTP credentials and relay configuration stay on the VPS.
- The default Kindle Email is configured on the VPS; the client never asks for it.
- The client stores its token in user storage and never writes Kindle system files.
- The install and uninstall hooks touch only the BookRelay launcher below /mnt/us; the package manager owns removal of the installed package directory.

Run the relay behind HTTPS. Do not expose SMTP credentials in a client build or commit a real .env file. Rotate or revoke the relay token if the Kindle user storage is copied.

The default SQLite database contains pairing records, device tokens in hashed form, Kindle e-mail addresses, and delivery metadata. It should be protected as private operational data. The current implementation keeps delivery EPUB bytes in process memory while sending and does not persist book contents.
