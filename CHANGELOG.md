# Changelog

## 0.1.0 — 2026-10-08

First bounded release of the local Syncthing conflict review workbench.

- Group conflict versions by original with text diff, bounded image preview and binary metadata.
- Explicit keep-original, use-conflict and keep-both plans; no automatic winner.
- SHA-256 revalidation, backups outside synced roots, local process exclusion and staged receipts.
- Conditional local undo/recovery with changed-state refusal.
- Disposable first-use demo, native keyboard-accessible controls and Korean quickstart.
- C++/Qt native packages and synthetic safety checks for Windows, macOS and Linux.

Pause Syncthing and close other writers before mutation. Operations are recoverable stages, not atomic multi-file transactions. No signing or notarization is provided.
