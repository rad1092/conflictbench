ConflictBench reviews Syncthing conflict copies in a user-selected local folder.
It offers explicit decisions, hash revalidation, external backups, receipts, and
limited local undo. Start with the built-in synthetic demo.

Pause Syncthing and close editors before applying or undoing a transaction.
Read README.md and SECURITY.md before working on important documents.

Packages: macOS Apple Silicon, Windows x86_64, and Ubuntu 24.04 x86_64.
Linux desktop dependencies are listed in LINUX-REQUIREMENTS.txt inside its archive.
The macOS package has an ad-hoc signature only; it is not Developer ID signed or
notarized. Windows binaries are not Authenticode signed. Operating systems may
show an unknown-publisher warning. Check SHA256SUMS before opening downloads.

The source archive and exact, unmodified Qtbase 6.11.2 corresponding source are
provided alongside the binaries. GPL/LGPL and third-party notices are included.
CI tests synthetic data; they do not establish safety for every filesystem or
recreate subsequent changes made by other devices.

The verification manifest links the exact release workflow. The verification
log archive preserves core/GUI tests, actual Linux disk-full recovery evidence,
and the secret/dependency reports beyond temporary CI artifact retention.
