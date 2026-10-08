# Privacy

ConflictBench's application code makes no network requests. No telemetry, automatic update checker, account, API key, sync-engine connection, or background service is implemented. It reads only the selected folder and the explicitly selected backup folder. It does not scan the home folder automatically.

Content hashes, filenames, absolute paths, sizes, permissions and transaction states are stored in local receipts beside the backup copies. These may be sensitive. Backups persist until you remove them yourself. The demo is temporary and cleaned up on normal exit. A crash can leave temporary demo/diff data in your OS temporary directory. Remove only directories you recognize as this app's disposable data.

Choosing an external diff program gives that program your account's normal permissions and the contents of verified temporary copies. The app cannot control that program's network activity or logging. Use trusted software. The snapshots are discarded when its process exits; application termination may stop a child tool.

No file contents should be included in issue reports. Public GitHub issue reports are public; redact paths, names and receipt metadata. Runtime dependency behavior is governed by its own notices. The app does not intentionally invoke network features of Qt.
