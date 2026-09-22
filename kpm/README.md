# BookRelay KPM repository

Add this repository once on a jailbroken Kindle with KPM:

~~~sh
kpm add-repo https://book.fffq.uk/i
kpm update
kpm install bookrelay-kindle
~~~

To install a later release, run:

~~~sh
kpm install bookrelay-kindle
~~~

To update every package known to KPM, run the command: kpm upgrade.

The repository contains the kindlehf build for Kindle Paperwhite 11 and 12
running firmware 5.19.0 or newer. The KPM package is delivered through a
GitHub Release and the release workflow publishes a SHA-256 checksum beside it.
