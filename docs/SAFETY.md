# Safety contract

ConflictBench operates on one selected original/conflict pair at a time. It is a local review tool, not a synchronization engine. Pause Syncthing for that folder and close all other file writers before committing or undoing. The app cannot verify that pause through its local-file mode.

## Supported inputs

Only explicitly selected local folders and plain regular files are actionable. The home directory and drive root are refused as scan roots. Symbolic links and Windows junctions/reparse points are rejected, including ancestors. Hard links, unsupported extended data/streams, special file modes, other-owner POSIX files, Windows hidden/system/encrypted/compressed/cloud attributes, and unsupported metadata are refused. macOS's OS-managed provenance attribute is allowed; resource forks, quarantine, Finder metadata and other xattrs are not silently stripped. POSIX extended ACLs are refused. Windows native owner/group/DACL are preserved and revalidated. Audit SACLs, mandatory integrity labels and security metadata beyond owner/group/DACL are not preserved or comprehensively detected; files whose policies depend on them are outside the supported scope. NTFS extended attributes, including WSL metadata, are refused.

The supported filesystem allowlist includes APFS/HFS, common local Linux filesystems, NTFS/ReFS and temporary test volumes. Passing the type check is not a durability certification. Network, FUSE and cloud placeholder workflows are unsupported. Scans skip .git, .stversions and .stfolder interiors, retain exact filename case, and report missing originals. A .sync-conflict suffix must match current Syncthing naming, including a valid date/time and device identifier. Case-only conflicts and deleted-original decisions require manual handling.

Limits are 100,000 entries, 1,000 conflict pairs and 256 MiB per file. Smaller folders are recommended. Previews hash the full file before showing at most 1 MiB. Inline text diff is limited to 1,200 lines; only UTF-8 is decoded as text. Images have allocation/pixel bounds. External diff snapshots are limited to 16 MiB and a truncated snapshot is never passed as a complete file.

## Commit and backup

A plan identifies both snapshots, the chosen decision, backup root and any keep-both name. A global per-user workbench lock serializes workbench transactions even if different backup folders or overlapping source roots are selected. It does not lock Syncthing or another editor.

Both originals are copied and SHA-256 verified inside a new private transaction directory before source mutation. The directory is outside the selected root, outside any recognized source .stfolder ancestor, and outside any .stfolder ancestor of the backup path. Custom Syncthing markers and unrelated sync programs cannot be discovered without configuration: the user must confirm the location is outside **all** synced folders. Backup contents and receipts stay until manually removed.

Before every write/remove boundary the app checks expected bytes, sizes, modification times and supported permissions/metadata. Atomic single-file replacement uses QSaveFile with direct-write fallback disabled. Source files are never directly truncated in place. The selected conflict is removed only after both backups and the chosen destination are verified.

The backup directory is owner-only on POSIX and has a protected per-user DACL on Windows. File backups are restricted within it. Parents with inheritable extended POSIX ACLs are refused. If the application cannot verify/create required permissions or flush/write data, it stops.

## Interruption and recovery

A durable receipt records stages before and after filesystem changes. This is **not an atomic multi-file operation**. Cancellation after a destination is written can leave a changed original plus its conflict; this is reported as interrupted. Use History / recovery before resuming sync. The recovery operation itself also records stages and can be retried after interruption.

Recovery verifies both backup hashes and current local states against the stage recorded in the receipt. Later unrecognized file contents, metadata changes, collisions, missing files or malformed receipts cause refusal. An OS destination rename may finish before its post-state is recorded. In that ambiguous `writing_destination` stage, automatic recovery is allowed only when both source snapshots are still pristine and no keep-both destination exists. Otherwise the app refuses and the verified backups remain available for manual recovery. The pause/no-other-writers requirement applies throughout commit, interruption and recovery. A hash match is not proof of a file's author or history.

Undo restores the saved bytes and supported access permissions; it is not a metadata-complete archival restore. A source document's original modification timestamp is recorded for validation, but restored files receive a new write time. Keep separate archival backups if broader metadata is required. It does not undo propagation or later edits on other devices.

The receipt is trusted local state in your private backup directory, not an authenticated document. Never accept somebody else's receipt as permission to restore a path. Path validation reduces accidental corruption; it is not a sandbox against a hostile process with your account's access. Hardware failures and power loss can defeat filesystem durability despite flushes; no universal guarantee is made.

## When recovery refuses

Keep synchronization paused and leave the backups/receipt intact. Copy backup files into a separate working directory and compare them with the current versions using an appropriate trusted editor. Do not edit a receipt to bypass checks, delete unexplained files, or repeatedly apply an old plan.
