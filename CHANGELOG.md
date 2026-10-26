# Changelog

## 0.2.0 — unreleased

- **Versions** highlights changed lines: red only in the original, green only in the conflict. The text difference tab uses the same colors.
- Conflicts are labelled with the date, time and device from their Syncthing names, and marked *identical* when their content matches the original.
- **Clean up identical copies** archives every identical conflict copy in one reviewed step. Each copy is still its own verified transaction with its own receipt; the batch stops at the first one that does not commit.
- The plan dialog starts with a one-sentence summary and a short table; full paths and SHA-256 are behind a toggle.
- The folder name is shown in bold, with the full path shortened beside it.
- The interface is translatable (`tr()`), with a Korean translation chosen from the system language. `--language` overrides it. Messages from the core library are still English.
- Internal review records are merged into `docs/VERIFICATION.md`; evidence files moved to `docs/evidence/`.

## 0.1.0 — 2026-10-08

First bounded release of the local Syncthing conflict review workbench.

- Group conflict versions by original with text diff, bounded image preview and binary metadata.
- Explicit keep-original, use-conflict and keep-both plans; no automatic winner.
- SHA-256 revalidation, backups outside synced roots, local process exclusion and staged receipts.
- Conditional local undo/recovery with changed-state refusal.
- Disposable first-use demo, native keyboard-accessible controls and Korean quickstart.
- C++/Qt native packages and synthetic safety checks for Windows, macOS and Linux.

Pause Syncthing and close other writers before mutation. Operations are recoverable stages, not atomic multi-file transactions. No signing or notarization is provided.
