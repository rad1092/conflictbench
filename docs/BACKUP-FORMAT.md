# Backup and receipt format 1

The user selects an existing backup root. Each transaction creates a private `conflictbench-<UUID>/` directory containing:

- `original.backup`: main byte stream of the original before resolution.
- `conflict.backup`: main byte stream of the selected conflict before resolution.
- `receipt.json`: UTF-8 JSON, format identifier `conflictbench-1`.

Backups deliberately have neutral names. They are not executable copies and should be copied to a separate working location with an appropriate extension for manual inspection. Never run an unknown file based on an extension alone.

The JSON records the canonical source root, absolute normalized paths, action number (`0` keep original, `1` use conflict, `2` keep both), optional keep-both destination, creation/update UTC timestamps and a state. Each snapshot records byte size as a decimal string, SHA-256 in lowercase hex, Qt permission flags, last-modified UTC and base64 native security metadata. Post-write and undo snapshots are added when those stages are recorded. Paths and metadata can be sensitive.

| State | Interpretation |
|---|---|
| `backing_up` | Backup creation has begun; source mutation has not been authorized. Backups may be incomplete. |
| `prepared` | Both backups were verified; source snapshots still matched. |
| `writing_destination` | Destination replacement/copy was authorized; it may or may not have completed. |
| `destination_written` | Destination bytes and post-state were recorded; selected conflict still existed. |
| `removing_conflict` | Conflict removal was authorized; it may or may not have completed. |
| `committed` | Chosen result and removal completed; backups remain. |
| `undo_started` | Local restore was authorized after validating current state and backup hashes. |
| `undo_conflict_restored` | Selected conflict was restored and recorded. |
| `undo_original_restored` | Original was restored; any keep-both copy may still need removal. |
| `undone` | Restore finished, or an unstarted backup stage was closed with pristine sources. |

A failed operation does not overwrite the last useful durable stage merely to label it `failed`. The UI can report `interrupted` or `recovery_required` while the receipt retains that last stage. No transaction is automatically retried on startup. Open History / recovery and explicitly confirm local undo. A `backing_up` record with unchanged sources can be closed without complete backups; later stages require both verified backups.

Receipt writes and destination writes use a temporary file and atomic replacement for each file, with direct-write fallback disabled. POSIX files/directories are flushed; Windows file data is flushed without claiming a portable directory-flush guarantee. A crash can leave OS temporary files. The app does not sweep unknown files or delete old backups automatically.

This is a versioned internal recovery format, not a public editing API. Keep the receipt and its two backups together. Removing or changing either can make automated recovery impossible. After verifying a result and independently backing up anything needed, users may remove an entire old transaction directory themselves; the application does not promise recovery afterward.
