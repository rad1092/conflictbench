# Support and limitations

Report reproducible defects at https://github.com/rad1092/conflictbench/issues. Include the app version, OS/filesystem, operation, error/state and a minimal synthetic example. Never upload private documents, backups, API keys or unredacted paths.

This independent project is not maintained or endorsed by Syncthing or Qt. Version 0.1 is a bounded first release: local regular files, one selected conflict pair per operation. Keep separate backups of important data. Pause Syncthing and other writers before commit or undo. No live-sync or distributed rollback guarantee is made.

Supported release targets: macOS arm64, Windows x86-64, and Linux x86-64. Actual tested OS versions and package results belong in release evidence, not inferred from compilation. Apple Intel, ARM Windows/Linux, network/FUSE filesystems, ACL/xattr/resource-fork preservation, cloud placeholders, FAT/exFAT durability, and case-conflict resolution are not qualified for this release. Packages are unsigned; macOS is not notarized.

If recovery refuses a changed state: leave the receipt and both backups intact, keep synchronization paused, copy the backups to a separate working directory, and compare the versions manually. Do not edit a receipt to bypass checks. An unresolved receipt is evidence, not permission to delete files.
