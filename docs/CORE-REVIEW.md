# Independent core safety review

Reviewed on 2026-10-08. Scope: `src/core.cpp` and `src/core.h`, with independent C++ probes against disposable files on macOS 27.0.1 arm64 / Qt 6.11.2. The reviewer did not edit application or production-test code. Findings were sent to the core implementation owner, who made the fixes.

**Result:** no reproduced local release blocker remains in the tested snapshot. This is a bounded code review and synthetic verification, not a security certification. Windows-specific behavior, Linux behavior, the packaged application and the final release commit still require their own recorded checks.

The independently tested source snapshot has these SHA-256 hashes:

| File | SHA-256 |
| --- | --- |
| `src/core.cpp` | `d81f9ba4adcd863ae80bb5ef4346d4d5fff79f7d43b18d0cfc2561b100b91674` |
| `src/core.h` | `7db4a354bcd433a38e80e65664451367cd480681e8e9c5b8bb80f2cd94341864` |

The [machine-readable evidence](core-review-evidence.json) records observations and these hashes. Later code changes are not implicitly covered by this snapshot; the final release must run the maintained tests again.

## Findings resolved during review

| Finding | Why it mattered | Fix and independent validation |
| --- | --- | --- |
| Recovery accepted known contents in an impossible transaction stage. | In `prepared`, no source write has occurred. Accepting an original that now equals conflict bytes, or an externally created keep-both destination, could undo a later user action. This was identified by inspection; the owner fixed it before the first compiled review probe. | Stage-specific checks and recorded post/undo snapshots were added. Independent probes confirmed refusal of both prepared-stage changes while preserving the files. A conflict deleted after `destination_written` is also refused instead of silently recreated. |
| The parser accepted multiple extensions after the conflict marker. | `report.sync-conflict-20260101-120000-ABCDEFG.txt.bak` was grouped with `report.txt.bak`. That is inconsistent with Syncthing's placement before the final extension and can mistake a manually retained backup for a conflict. The old parser's return value was reproduced. | The suffix now allows only the final extension. The same probe returns no match. Production parser fixtures also cover valid compound extensions, dotfiles, Unicode and extensionless names. |
| macOS resource-fork data was lost. | A synthetic conflict containing data bytes plus the `com.apple.ResourceFork` value `FORK` could be accepted successfully. The conflict was deleted, while both the replacement and backup lacked the fork. | Unsupported extended attributes/resource forks are now refused before mutation. Independent scan returned zero actionable pairs, retained the conflict, and retained all four resource-fork bytes. Metadata added after scanning is checked again by the core. |
| Private backup modes did not override an inherited macOS ACL. | A synthetic backup parent granting inherited everyone read/search access produced a transaction directory with mode 0700 and a backup with mode 0600 that still carried the broad inherited ACL. Permission bits alone did not make those backups private. | Extended directory ACLs are now rejected before creating a transaction. The same inherited-ACL fixture is refused with no successful operation. Windows uses an explicit protected per-user directory DACL; its runtime behavior requires Windows tests. |
| Receipt paths could escape the selected root through `..`. | A modified synthetic receipt with `<root>/../outside/report.txt` passed the former lexical prefix check. With matching recorded post-state, undo changed the outside file and created a conflict beside it. | Decoded snapshot paths must be normalized. The identical probe now refuses the receipt, preserves outside bytes `BBBB`, and creates no outside conflict. All “outside” paths in this experiment remained inside one disposable test parent. |

Two platform details found while checking the metadata fix were also resolved. This host automatically adds `com.apple.provenance` to ordinary newly created files, so rejecting every xattr made normal scans unusable. Only that OS-managed provenance attribute is now tolerated; user data attributes and resource forks remain unsupported. Also, this Darwin runtime reports a missing extended ACL as a null result with `ENOENT` even when the file exists. The implementation distinguishes that case from other inspection failures. No real file was stripped of metadata to make the tests pass.

## Additional independent checks

The final temporary probe ran eleven cases. Besides the fixed findings above, it confirmed:

- Changing either source or destination to different bytes of identical length, then restoring the reviewed modification time, causes execution to refuse. The later bytes remain. A stale original preview is refused too.
- Replacing an original with a symlink after scanning causes execution to refuse; the synthetic target outside the selected folder remains `SAFE`.
- An uppercase alias of an inside-root backup directory on this case-insensitive macOS filesystem is canonicalized and rejected. The alias is not treated as an outside backup.

Relevant maintained regressions are in [core_tests.cpp](../tests/core_tests.cpp), including `preparedStageRejectsLaterRecognizedBytesAndNewKeptCopy`, `receiptPathTraversalRefused`, `extendedDataStreamsRefusedBeforeBackup`, `metadataAddedAfterScanRefused`, `inheritableBackupAclRefused`, `sameSizeMutationWithRestoredTimestamp`, `sourceMutationAtCommitBoundaryAborts`, `crashRestartRecovery` and `interruptedUndoCanResume`. The core owner's full-suite result is separate from the independent eleven-case evidence; this review does not claim that every maintained test was independently rerun here.

The probe compiled copied core sources and a small harness in a dedicated temporary directory, at low concurrency. All document contents, resource forks, ACLs, symlinks, receipt modifications and backup folders were synthetic. The temporary source copies, executables and fixture directories were removed after recording results. The shared test-mode transaction-lock directory was not removed because other test processes may use it.

## Windows handle and security review

`QFileDevice::handle()` is a C-runtime integer descriptor, not a raw Win32 `HANDLE`. The `_get_osfhandle` conversion before `FlushFileBuffers` is appropriate. This is supported by [Qt's handle documentation](https://doc.qt.io/qt-6/qfiledevice.html#handle) and the [Qt 6.8 Windows implementation](https://github.com/qt/qtbase/blob/6.8/src/corelib/io/qfsfileengine_win.cpp#L345-L358), which obtains the descriptor using `_open_osfhandle`.

Qt also explicitly warns that [`setPermissions()` does not manipulate ACLs](https://doc.qt.io/qt-6/qfiledevice.html#setPermissions). The revised Windows path therefore records native owner/group/DACL information, applies it to the prepared replacement, and refuses unsupported alternate streams and special file attributes. Code inspection alone does not establish that `ReOpenFile`, owner restoration, protected/inherited DACL behavior and flushing work under the release toolchain. Windows execution of successful actions, undo, ACL preservation and ADS refusal remains a release gate.

## Limits that must remain visible

- Stop Syncthing and other writers for the affected files. Hashes and a workbench lock do not exclude a separate process writing in the final interval between validation and rename/removal. The implementation is not a sandbox against a hostile local process.
- A transaction spans several files. Its durable stage and verified backups support interruption recovery; this is not an atomic multi-file transaction or a guarantee against power failure, hardware failure, network filesystem behavior, or every filesystem's durability semantics.
- Local-file mode detects the selected folder and known `.stfolder` ancestors. It cannot discover every custom Syncthing marker, other configured sync root, or another sync program. Users must choose a backup location outside all such folders.
- Undo is local and conditional on recognized current state. It cannot reverse later propagation or edits on other devices.
- Supported plain regular files are a deliberate restriction. Refusing unsupported metadata is necessary to avoid claiming that copying only the main byte stream preserves every document. macOS provenance may be regenerated by the operating system; this is not an attribute-preserving archival tool.
- macOS `/tmp` and `/var` aliases can be refused by the no-symlink policy. Tests and the demo should use canonical paths; weakening path checks to accept arbitrary symlinks would undermine this boundary.
