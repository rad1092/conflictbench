# ConflictBench

A local desktop workbench for reviewing Syncthing conflict files before resolving them. Compare text, images, or binary metadata, make an explicit decision, and keep verified backups and a recoverable transaction receipt outside the synced folder.

**Why install it?** A conflict list or “newest wins” rule cannot tell you which version contains the change you intended. ConflictBench ties each decision to reviewed SHA-256 hashes, rechecks files before mutation, and records interrupted stages instead of claiming an all-or-nothing multi-file transaction. [The alternatives study](docs/RESEARCH.md) includes a reproducible synthetic comparison against a small existing resolver.

No account, Syncthing API key, daemon, telemetry, or automatic winner. ConflictBench is independent of the Syncthing project.

## Start

Download your platform package from [Releases](https://github.com/rad1092/conflictbench/releases). Extract it and open **ConflictBench**. The packages are unsigned and macOS is not notarized. Verify the published SHA-256 checksum first. Linux runtime requirements and build instructions are in [BUILDING](docs/BUILDING.md).

Choose **Try demo** for disposable text, Korean Unicode, image, and binary examples. Demo files and backups are removed when the application closes. To preserve real work, use your own selected folder and a separate private backup location.

1. **Open folder** scans only the folder you choose. Scanning does not alter its files.
2. Select a conflict under its original. Inspect **Versions** and **Text difference**.
3. Choose **Keep original**, **Use conflict**, or **Keep both**. Each decision resolves only the selected conflict version.
4. Choose an existing private backup folder outside **every** synced folder. Pause Syncthing for the selected folder and close other programs writing those files.
5. **Review plan** displays the affected paths and hashes. Check both acknowledgments to enable commit.
6. Verify the result before resuming synchronization. **History / recovery** opens your backup folder and can restore recognized local states.

[한국어 빠른 시작](docs/QUICKSTART.ko.md) · [Safety and recovery](docs/SAFETY.md) · [Backup format](docs/BACKUP-FORMAT.md) · [Privacy](docs/PRIVACY.md) · [Support](SUPPORT.md)

## What the decisions do

| Decision | Result after backups are verified |
|---|---|
| Keep original | Original remains; the selected conflict is removed from the sync folder and retained in the external backup. |
| Use conflict | Original receives the selected conflict's bytes; selected conflict is removed. Both prior versions stay in backup. |
| Keep both | Original remains; a separately named copy retains the conflict's extension. The conflict-marker file is removed. |

Conflicts missing their original are skipped because deletion may have been intentional. Case-only name collisions are a different Syncthing problem and are not resolved here. Dates and sizes are informative; they never determine a winner.

## Safety boundaries

Local regular files only, maximum 256 MiB per version, 100,000 scanned entries and 1,000 conflict pairs. UTF-8 text preview is limited to 1 MiB, inline diff to 1,200 lines; image decoding is bounded. Unsupported formats get metadata and an explicit decision. A trusted external diff executable receives temporary verified snapshots using a literal argument list, without a shell.

**Pause is required.** Hash checks detect observed changes but cannot prevent a writer changing a file after its final check. Use a local filesystem with normal atomic rename semantics. Network drives, cloud placeholders, malicious concurrent writers, and preserving ACLs/extended attributes/resource forks are outside this release's supported scope. Undo is local and conditional; it does not reverse later changes on other devices. Read [the complete contract](docs/SAFETY.md) before using important documents.

## Development

C++17, CMake, Qt 6.8 or later; release builds use Qt 6.11.2. Qt is dynamically linked. There is no JavaScript or server component. Build/test instructions, dependency sources, license notices, and packaging commands are in [BUILDING](docs/BUILDING.md) and [LICENSING](docs/LICENSING.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

GPL-3.0-only. Third-party libraries retain their own licenses. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES.md).
