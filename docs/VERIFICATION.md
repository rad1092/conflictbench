# Verification records

How ConflictBench was reviewed and tested. These are maintainer records; for using the app, start with the [README](../README.md), [first run](FIRST-RUN.md) and [safety contract](SAFETY.md).

- [Core review](#core-review)
- [Final safety review](#final-safety-review)
- [Secret scan review](#secret-scan-review)
- [GUI tests](#gui-tests)
- [Native GUI runs](#native-gui-runs)
- [Disk-full test](#disk-full-test)

## Core review

Reviewed on 2026-10-08. Scope: `src/core.cpp` and `src/core.h`, with independent C++ probes against disposable files on macOS 27.0.1 arm64 / Qt 6.11.2. The reviewer did not edit application or production-test code. Findings were sent to the core implementation owner, who made the fixes.

**Result:** no reproduced local release blocker remains in the tested snapshot. This is a bounded code review and synthetic verification, not a security certification. Windows-specific behavior, Linux behavior, the packaged application and the final release commit still require their own recorded checks.

The independently tested source snapshot has these SHA-256 hashes:

| File | SHA-256 |
| --- | --- |
| `src/core.cpp` | `d81f9ba4adcd863ae80bb5ef4346d4d5fff79f7d43b18d0cfc2561b100b91674` |
| `src/core.h` | `7db4a354bcd433a38e80e65664451367cd480681e8e9c5b8bb80f2cd94341864` |

The [machine-readable evidence](evidence/core-review-evidence.json) records observations and these hashes. Later code changes are not implicitly covered by this snapshot; the final release must run the maintained tests again.

### Findings resolved during review

| Finding | Why it mattered | Fix and independent validation |
| --- | --- | --- |
| Recovery accepted known contents in an impossible transaction stage. | In `prepared`, no source write has occurred. Accepting an original that now equals conflict bytes, or an externally created keep-both destination, could undo a later user action. This was identified by inspection; the owner fixed it before the first compiled review probe. | Stage-specific checks and recorded post/undo snapshots were added. Independent probes confirmed refusal of both prepared-stage changes while preserving the files. A conflict deleted after `destination_written` is also refused instead of silently recreated. |
| The parser accepted multiple extensions after the conflict marker. | `report.sync-conflict-20260101-120000-ABCDEFG.txt.bak` was grouped with `report.txt.bak`. That is inconsistent with Syncthing's placement before the final extension and can mistake a manually retained backup for a conflict. The old parser's return value was reproduced. | The suffix now allows only the final extension. The same probe returns no match. Production parser fixtures also cover valid compound extensions, dotfiles, Unicode and extensionless names. |
| macOS resource-fork data was lost. | A synthetic conflict containing data bytes plus the `com.apple.ResourceFork` value `FORK` could be accepted successfully. The conflict was deleted, while both the replacement and backup lacked the fork. | Unsupported extended attributes/resource forks are now refused before mutation. Independent scan returned zero actionable pairs, retained the conflict, and retained all four resource-fork bytes. Metadata added after scanning is checked again by the core. |
| Private backup modes did not override an inherited macOS ACL. | A synthetic backup parent granting inherited everyone read/search access produced a transaction directory with mode 0700 and a backup with mode 0600 that still carried the broad inherited ACL. Permission bits alone did not make those backups private. | Extended directory ACLs are now rejected before creating a transaction. The same inherited-ACL fixture is refused with no successful operation. Windows uses an explicit protected per-user directory DACL; its runtime behavior requires Windows tests. |
| Receipt paths could escape the selected root through `..`. | A modified synthetic receipt with `<root>/../outside/report.txt` passed the former lexical prefix check. With matching recorded post-state, undo changed the outside file and created a conflict beside it. | Decoded snapshot paths must be normalized. The identical probe now refuses the receipt, preserves outside bytes `BBBB`, and creates no outside conflict. All “outside” paths in this experiment remained inside one disposable test parent. |

Two platform details found while checking the metadata fix were also resolved. This host automatically adds `com.apple.provenance` to ordinary newly created files, so rejecting every xattr made normal scans unusable. Only that OS-managed provenance attribute is now tolerated; user data attributes and resource forks remain unsupported. Also, this Darwin runtime reports a missing extended ACL as a null result with `ENOENT` even when the file exists. The implementation distinguishes that case from other inspection failures. No real file was stripped of metadata to make the tests pass.

### Additional independent checks

The final temporary probe ran eleven cases. Besides the fixed findings above, it confirmed:

- Changing either source or destination to different bytes of identical length, then restoring the reviewed modification time, causes execution to refuse. The later bytes remain. A stale original preview is refused too.
- Replacing an original with a symlink after scanning causes execution to refuse; the synthetic target outside the selected folder remains `SAFE`.
- An uppercase alias of an inside-root backup directory on this case-insensitive macOS filesystem is canonicalized and rejected. The alias is not treated as an outside backup.

Relevant maintained regressions are in [core_tests.cpp](../tests/core_tests.cpp), including `preparedStageRejectsLaterRecognizedBytesAndNewKeptCopy`, `receiptPathTraversalRefused`, `extendedDataStreamsRefusedBeforeBackup`, `metadataAddedAfterScanRefused`, `inheritableBackupAclRefused`, `sameSizeMutationWithRestoredTimestamp`, `sourceMutationAtCommitBoundaryAborts`, `crashRestartRecovery` and `interruptedUndoCanResume`. The core owner's full-suite result is separate from the independent eleven-case evidence; this review does not claim that every maintained test was independently rerun here.

The probe compiled copied core sources and a small harness in a dedicated temporary directory, at low concurrency. All document contents, resource forks, ACLs, symlinks, receipt modifications and backup folders were synthetic. The temporary source copies, executables and fixture directories were removed after recording results. The shared test-mode transaction-lock directory was not removed because other test processes may use it.

### Windows handle and security review

`QFileDevice::handle()` is a C-runtime integer descriptor, not a raw Win32 `HANDLE`. The `_get_osfhandle` conversion before `FlushFileBuffers` is appropriate. This is supported by [Qt's handle documentation](https://doc.qt.io/qt-6/qfiledevice.html#handle) and the [Qt 6.8 Windows implementation](https://github.com/qt/qtbase/blob/6.8/src/corelib/io/qfsfileengine_win.cpp#L345-L358), which obtains the descriptor using `_open_osfhandle`.

Qt also explicitly warns that [`setPermissions()` does not manipulate ACLs](https://doc.qt.io/qt-6/qfiledevice.html#setPermissions). The revised Windows path therefore records native owner/group/DACL information, applies it to the prepared replacement, and refuses unsupported alternate streams and special file attributes. Code inspection alone does not establish that `ReOpenFile`, owner restoration, protected/inherited DACL behavior and flushing work under the release toolchain. Windows execution of successful actions, undo, ACL preservation and ADS refusal remains a release gate.

### Limits that must remain visible

- Stop Syncthing and other writers for the affected files. Hashes and a workbench lock do not exclude a separate process writing in the final interval between validation and rename/removal. The implementation is not a sandbox against a hostile local process.
- A transaction spans several files. Its durable stage and verified backups support interruption recovery; this is not an atomic multi-file transaction or a guarantee against power failure, hardware failure, network filesystem behavior, or every filesystem's durability semantics.
- Local-file mode detects the selected folder and known `.stfolder` ancestors. It cannot discover every custom Syncthing marker, other configured sync root, or another sync program. Users must choose a backup location outside all such folders.
- Undo is local and conditional on recognized current state. It cannot reverse later propagation or edits on other devices.
- Supported plain regular files are a deliberate restriction. Refusing unsupported metadata is necessary to avoid claiming that copying only the main byte stream preserves every document. macOS provenance may be regenerated by the operating system; this is not an attribute-preserving archival tool.
- macOS `/tmp` and `/var` aliases can be refused by the no-symlink policy. Tests and the demo should use canonical paths; weakening path checks to accept arbitrary symlinks would undermine this boundary.

## Final safety review

Reviewed 2026-10-08 on macOS 27.0.1 arm64, Apple Clang 21.0.0 and shared Qt 6.11.2. This supplements the [core review](#core-review). Scope: transaction stages, conditional recovery, receipt paths, cancellation, and Windows metadata handling in `core.cpp`, with maintained tests and disposable synthetic probes. It does not certify GUI/package behavior or substitute for the final commit's three-OS CI.

**Result:** the reproduced recovery blocker below is fixed in the tested snapshot. The independent macOS probe confirms refusal without overwriting or removing the later files. The maintained core suite built from the same copied sources reports **64 passed, 0 failed, 2 Windows-only skips**. Windows EA and native ACL/junction behavior still require successful Windows CI; no Windows runtime result is claimed here.

| Independently built file | SHA-256 |
| --- | --- |
| `src/core.cpp` | `9e05e04c6eacd7e0783b0f983e675214e7fdda7f59f8a55c4c6c2cfa221a6b81` |
| `src/core.h` | `7db4a354bcd433a38e80e65664451367cd480681e8e9c5b8bb80f2cd94341864` |
| `tests/core_tests.cpp` | `9c27b91ba841d4cf1d051e66e91d6c0e27a029d73af91adb9b5fbfa12918f353` |

Later source changes are not implicitly covered. The review began before the initial Git commit, so hashes identify the actual snapshot. Builds used one compiler process and copied sources in a dedicated temporary directory. No personal documents were scanned or changed.

### Reproduced and resolved recovery blocker

In the initial snapshot (`core.cpp` SHA-256 `e48f9b7ef97c28b0ec788f298923769ec5cd007991de8fc6d156f6a64fe3ce64`), `writing_destination` recovery accepted either known original contents, or a keep-both destination whose bytes matched the conflict, without a recorded post-write snapshot. This was too permissive even without receipt tampering.

Reproduction used one original containing `old version\n` and one conflict containing `new version\n`. A fault on the third `before_copy_commit` interrupted execution after both backups, but before destination rename. The durable state was `writing_destination`; the original was still unchanged. The probe then simulated a later user's action by writing the conflict bytes to the original, or creating the planned keep-both destination, and advancing its modification time by seven seconds.

| Synthetic later action | Initial recovery result | Retest after fix |
| --- | --- | --- |
| Replace original with the conflict bytes | Returned success and replaced the later original with old bytes | Refused recovery; preserved `new version\n` |
| Create the planned keep-both file with conflict bytes | Returned success and deleted the later file | Refused recovery; file remained with `new version\n` |

The implementation owner changed this stage to require both exact pre-operation snapshots and an absent keep-both destination. A crash after rename but before durable post-state now requires manual recovery from the verified backups. This is intentional: matching content cannot identify who wrote the file. Maintained regressions are `unrecordedDestinationRefusesLaterMatchingWrite` for both actions.

Other destructive undo windows were inspected. A possibly restored conflict is accepted without rewriting it. An original already containing the old bytes is likewise left intact. Removal of a keep-both copy during resumed undo still requires its recorded undo-start snapshot. No additional destructive-stage blocker was reproduced.

### Windows extended attributes

Code inspection found that the Windows branch refused alternate data streams but did not inspect NTFS extended attributes. These are different metadata facilities: [FindFirstStreamW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirststreamw) enumerates `$DATA` streams, while [FILE_EA_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_ea_information) reports application-specific extended attributes. A practical example is [WSL permission metadata](https://learn.microsoft.com/en-us/windows/wsl/file-permissions), including `$LXUID`, `$LXGID` and `$LXMOD`. A byte-only replacement does not preserve those values.

With the implementation owner's coordination, this reviewer added a fail-closed query on the already opened identity handle. It requests `FILE_READ_EA` and uses the documented [NtQueryInformationFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntqueryinformationfile) with [FileEaInformation, value 7](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_file_information_class). The function is resolved only from already loaded system `ntdll.dll`. There is no DLL search, privilege enabling or fallback that ignores a failed query. If the function is missing, the result fails or is too short, or the EA size is nonzero, the file is refused before mutation. Every subsequent snapshot repeats that inspection.

`windowsExtendedAttributesRefused` adds a synthetic EA using the native equivalent of [ZwSetEaFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwseteafile), then checks original and conflict separately, both before scanning and after reviewing a plan. It asserts refusal and unchanged main bytes, with no transaction created. This test is Windows-only. **The reviewer implemented this patch; it is not independently runtime-validated by the macOS result.** Successful Windows compilation and execution are release gates.

Windows owner/group/DACL preservation, alternate-stream rejection, private backup DACLs and junction checks already have maintained tests. Audit SACLs, mandatory integrity labels and other security metadata beyond owner/group/DACL are not preserved or comprehensively detected. Files relying on such policies must remain outside the supported scope; do not market this as a complete Windows security-descriptor archive.

### Other executed checks and limits

The independent four-case probe also confirmed that cancellation after a recorded destination leaves a recoverable interrupted transaction, and that interruption after undo's original restoration can be retried. Both original and conflict bytes were restored in those cases.

The full maintained suite in the copied snapshot additionally passed normalized receipt traversal refusal, source and preview revalidation, same-size mutation with restored timestamp, symlink/ancestor/hard-link refusal, backup placement and permissions, malformed receipts, external changes, corrupt backups, crash/restart and transaction serialization. The two skips were Windows EA and native permissions/junction tests.

Receipts remain trusted private local state, not authenticated instructions. Normalization prevents the demonstrated `..` escape; it does not authenticate a receipt deliberately rewritten to identify another root and matching files. Users must not import somebody else's receipt or edit one to bypass a refusal.

Pause Syncthing and all other writers throughout commit and recovery. Hashes, permission checks and the per-user lock cannot exclude a noncooperating process in the final validation-to-rename/remove interval. Multi-file operations are not atomic, and file/directory flushes do not guarantee survival of every power-loss or filesystem failure. Windows has no claimed portable directory-flush guarantee. Recovery is local, conditional, and does not reverse propagation to other devices. Full metadata preservation and original modification-time restoration are not claimed.

Temporary copied source, custom probe, build products and disposable fixtures were removed after these results were recorded. The shared Qt test-mode lock directory was left alone because other test processes may use it.

### CI follow-up: private lock bootstrap

The release owner subsequently reported that actual macOS 15 CI could not create the transaction lock beneath its ACL-bearing `Library/Application Support` parent. Core tests used Qt's test-mode directory, so that host-specific startup condition had not been covered by their earlier success. This reviewer implemented a separate internal lock bootstrap; backup directory creation retains its original conservative parent-ACL refusal.

The bootstrap creates a new empty lock directory with mode 0700 on POSIX, opens that directory without following a leaf symlink, and removes only its own inherited ACL through the descriptor. macOS uses an empty extended ACL; Linux removes inherited access/default ACL xattrs. It reapplies mode 0700 and verifies directory ownership, mode and absence of extended ACLs before creating a lock. Existing directories are validated and refused if unsuitable; they are never repaired automatically. No parent, source or backup directory ACL is changed. The production location remains one fixed global per-user directory regardless of selected source or backup roots.

Windows continues to create a protected DACL granting the current user full access. The creation descriptor now explicitly specifies that user's SID as owner, avoiding an elevated token's potentially different default owner. Existing lock directories are checked for the current owner and a protected, single-user DACL. `windowsLockBootstrapKeepsProtectedPerUserAcl` verifies creation beneath a broader parent, safe reuse, and refusal without mutation after an existing directory's DACL is broadened. Windows runtime execution remains a CI requirement.

`lockBootstrapClearsOnlyNewDirectoryAcl` passed on macOS with a synthetic parent containing an inheritable everyone-read/search ACL: the new child had mode 0700 with no extended ACL, the parent's ACL was byte-for-byte unchanged in its text representation, safe reuse succeeded, and an existing broader directory was refused without alteration. The pre-existing backup ACL refusal test also passed. The Linux variant creates a synthetic default POSIX ACL and checks that the child loses both access/default ACL xattrs while the parent retains its default ACL; its execution belongs to Linux CI.

Syncthing control filenames and temporary prefixes are now excluded case-insensitively on all platforms, including `.STIGNORE`, `.SYNCTHING.*` and `~SYNCTHING~*`, to avoid aliases of engine control files on case-insensitive volumes. Exact-case pairing of ordinary documents is unchanged.

After these changes, a separate single-process macOS build of the maintained core suite reported **68 passed, 0 failed, 3 Windows-only skips**. The three skips were protected lock DACL, extended attributes, and native permissions/junction tests. The tested hashes are `d9480a90a79e1046eac34dac885abe545853e4aada585108c14bb54fac5819fc` for `src/core.cpp` and `523afecc1b585f58458502f608a3ec3fefd70dc96dfe0b00fa3996f899c23749` for `tests/core_tests.cpp`. The temporary follow-up build was removed. A passing macOS 15 application smoke and the final multi-OS release commit's CI must be recorded separately.

## Secret scan review

CI run [37718776547](https://github.com/rad1092/conflictbench/actions/runs/37718776547)
reported one Gitleaks 8.30.1 `generic-api-key` finding at
`docs/FINAL-SAFETY-REVIEW.md:34`, commit
`bebe83e50f31002bdc2d29c4e414bd5b65e0761c`.

The line is prose explaining refusal when a Windows native function is missing
or an extended-attribute query fails. It contains public Microsoft documentation
links and ordinary English, not a credential, token, or synthetic secret.
The redacted scanner report's match begins with the word “API” followed by a
comma; the following failure-description prose was misclassified as its value.

`.gitleaksignore` contains only this commit/file/rule/line fingerprint, using
[Gitleaks' documented fingerprint mechanism](https://github.com/gitleaks/gitleaks#-gitleaksignore).
No whole commit, path, rule, or pattern is excluded. The current paragraph was
rephrased to avoid repeating the ambiguous wording. Every other finding still
fails CI, and the complete repository history continues to be scanned with
redacted output.

## GUI tests

`tests/gui_tests.cpp` drives real Qt widgets and modal transaction dialogs using
QtTest. All source and backup fixtures come from the application's disposable
demo or isolated test directories under the system temporary directory. The
transient transaction lock uses Qt's test-mode data location. The suite never
opens or edits a personal sync folder.

The `desktop_workflow` CTest target runs with `QT_QPA_PLATFORM=offscreen`, including
on Windows, macOS and Linux CI. It does not require a desktop session, Syncthing,
an account, a network service, or an external diff executable.

Covered behavior:

- Keyboard activation of **Try demo** produces three original/conflict groups.
  Korean text and a changed line render correctly, images have decoded previews,
  and binary files require an explicit decision.
- Selecting a group instead of a conflict clears previews and disables actions.
- The plan shows paths, SHA-256 hashes, backup location, and the multi-file
  interruption limitation. Commit remains disabled with neither acknowledgment
  or only one acknowledgment; clearing an acknowledgment disables it again.
- Cancel preserves every demo directory entry and file hash and creates no
  transaction receipt.
- Each explicit decision commits only the selected conflict, preserves the
  expected original/new content, removes the conflict name, and records a receipt
  that appears in the history dialog. Undo starts disabled in history. The saved
  receipt is also exercised through the core rollback API.
- A same-size edit after scanning invalidates the preview and disables review
  and external diff. A same-size edit while the plan is open is rejected at
  commit, preserves the changed original, and leaves all conflicts unresolved.
- Equal-size files with the same first 1 MiB and different tails cannot appear
  completely compared: truncated text refuses inline diff, warns above the
  decision controls and final plan, and reports different whole-file hashes.
  Cancelling that plan preserves every source and backup hash.
- Opening a second synthetic sync folder after the demo requires a new backup
  selection. It cannot silently reuse the demo's disposable backup directory.
- Selecting a synthetic non-executable file as the external tool reports launch
  failure and leaves all source files and transaction history unchanged.

The suite intentionally uses Qt's file-picker implementation for deterministic
history navigation. It does not claim to verify OS-native file pickers, screen
reader announcements, monitor scaling, notarization, actual Syncthing pause
state, or a live multi-device sync race. Those require the separate manual/native
smoke record and the stated product limits. QtTest does not automate an actual
user desktop during these tests.

Run after configuring with `BUILD_TESTING=ON`:

```sh
cmake --build build --target gui_tests --parallel 2
ctest --test-dir build --output-on-failure -R desktop_workflow
```

Modal automation has a 15-second safety timeout. A timeout is a test failure,
not a successful cancellation or verification result. File permission failures,
disk-full injection, interrupted transaction recovery, locking, parser edge
cases, and symlink rejection belong to `core_safety`, not this desktop suite.

## Native GUI runs

On 2026-10-08, the complete `gui_tests` suite passed using Qt's native Cocoa
platform plugin on macOS 27.0.1 / Apple Silicon, Qt 6.11.2 (shared libraries),
Apple LLVM 21.0.0. All files were generated demo fixtures or isolated temporary
test files. No personal folder, live Syncthing instance, or desktop capture was
used.

| Run | Result | Duration |
| --- | --- | --- |
| Native Cocoa, default scale | 13 passed, 0 failed, 0 skipped | 6.279 s |
| Native Cocoa, `QT_SCALE_FACTOR=2` | 13 passed, 0 failed, 0 skipped | 4.529 s |
| Offscreen regression | 13 passed, 0 failed, 0 skipped | 0.846 s |

The count includes QtTest initialization and cleanup. These are working-tree
checks based on `bebe83e50f31002bdc2d29c4e414bd5b65e0761c` plus the partial-preview
safety UI fix under review; the release workflow must verify the final
commit separately. The final local run was at 02:48 UTC with source SHA-256:

| Source | SHA-256 |
| --- | --- |
| `src/core.cpp` | `d9480a90a79e1046eac34dac885abe545853e4aada585108c14bb54fac5819fc` |
| `src/workbench.cpp` | `963cc2d9a158bb28943f7d1de117a0ba47b9110b4b399dbeb41dea625988501a` |
| `tests/gui_tests.cpp` | `7a039467645fb37bcf756d12d858aa1eeee7453e0b64e51dc19465821454fa99` |

The accompanying screenshot files preserve the result of this run, not a claim
about later builds.

The suite exercises keyboard demo activation; Korean Unicode text and filenames;
decoded image and binary previews; all three explicit decisions; acknowledgment
gating and cancel-without-writes; receipt visibility; same-size source changes
before review and while review is open; a new backup selection after leaving
the demo; and external executable launch failure. Every decision is checked
against resulting file bytes and the receipt; rollback of each receipt is
checked using the core API. See [the full coverage description](#gui-tests).

A dedicated regression selects the last index in the scan vector, commits it,
then verifies that the shortened scan has a valid new selection. It compares
every remaining directory entry and file hash, checks the receipt's original
path, and checks that review remains enabled. This covers the stale selection
index found when Linux sorted the Korean filename last; it does not rely on an
OS-specific filename sort order.

A partial-preview regression uses equal-size files with an identical 1 MiB
prefix and different `OLD` / `NEW` tails. The prefix has only 1,024 newlines,
so the independent 1,200-line limit cannot conceal the byte-limit regression.
The test checks that inline diff is refused, the whole-file hashes differ,
both tabs retain a visible scope warning, and the final plan explicitly says
the decision affects undisplayed content. Cancel preserves all source and
backup directory hashes and creates no receipt.

### Captures

The optional `CB_GUI_CAPTURE_DIR` environment variable saves only a synthetic
application widget with `QWidget::grab()`. It never captures the desktop or other
applications. A failed requested capture fails the test. Default test runs save
no images. Each capture logs its pixel size and device pixel ratio.

Linux (Xvfb, `xcb`) captures of the current interface, from the same tests: [text](screenshots/xcb-text.png), [image](screenshots/xcb-image.png), [binary](screenshots/xcb-binary.png), [plan](screenshots/xcb-plan.png), [history](screenshots/xcb-history.png), [partial difference](screenshots/xcb-partial-text-difference.png), [partial plan](screenshots/xcb-partial-plan.png). The macOS captures below predate line highlighting and the plan summary.

- [Text and Korean filename](screenshots/cocoa-text.png): 1180 × 820, DPR 1.
- [Image versions](screenshots/cocoa-image.png): 1180 × 820, DPR 1.
- [Binary explicit-choice guidance](screenshots/cocoa-binary.png): 1180 × 820, DPR 1.
- [Plan and initially disabled commit](screenshots/cocoa-plan.png): 720 × 540, DPR 1.
- [Committed receipt and disabled undo](screenshots/cocoa-history.png): 850 × 520, DPR 1.
- [Text at Qt scale factor 2](screenshots/cocoa-text-scale2.png): 1920 × 1200, DPR 2.
- [Partial preview cannot imply a complete comparison](screenshots/cocoa-partial-text-difference.png): 1180 × 820, DPR 1.
- [Partial scope remains visible in the final plan](screenshots/cocoa-partial-plan.png): 720 × 540, DPR 1.

All eight refreshed captures were visually inspected. Both image versions now fit
inside their preview panes; binary guidance wraps and is fully readable; long
metadata filenames are deliberately elided with the full value in a tooltip.
Long plan paths remain horizontally scrollable. The comparison-scope label fits
below both tabs and above the plan contents, including with partial previews.
The scale-2 run
reported Qt window-placement warnings when a requested logical window exceeded
the available screen area, but all workflow assertions passed. This checks
Qt's scaled rendering and interaction, not moving a physical window between
monitors.

### Picker harness repair

The first native run had four failures: all three history checks showed zero
rows and the external-tool check timed out. Instrumenting the file picker's
`filesSelected` signal showed that `selectFile()` during Cocoa modal setup left
history pointing at the demo's parent directory and left the executable
selection empty. The file operations and receipt creation had passed.

The corrected harness enters the absolute path in the real Qt file-name field,
accepts the picker normally, and independently asserts the selected canonical
path. The original receipt-count, file-content, rollback, and error assertions
remain in place. `AA_DontUseNativeDialogs` is now set before constructing
`QApplication`; moving the attribute alone did not fix the selection failures.

The window platform is Cocoa, but file pickers deliberately remain Qt widgets
for deterministic automation. These tests do not verify Apple's native file
picker, VoiceOver announcements, native keyboard-only picker navigation, GUI
rollback confirmation, code signing, notarization, actual Syncthing pause state,
or remote-device changes. They do not replace the safety limitations in
[SAFETY.md](SAFETY.md).

### Reproduction

Use a logged-in macOS GUI session with the configured Qt build. These commands
bound the entire test process to 60 seconds; modal helpers additionally fail
after 15 seconds.

```sh
cmake --build build --target gui_tests --parallel 2
CB_GUI_CAPTURE_DIR=docs/screenshots python3 -c 'import subprocess; r = subprocess.run(["./build/gui_tests", "-o", "/tmp/conflictbench-native-gui-tests.txt,txt"], timeout=60); raise SystemExit(r.returncode)'
QT_SCALE_FACTOR=2 CB_GUI_CAPTURE_DIR=/tmp/conflictbench-native-hidpi python3 -c 'import subprocess; r = subprocess.run(["./build/gui_tests", "-o", "/tmp/conflictbench-native-hidpi-tests.txt,txt"], timeout=60); raise SystemExit(r.returncode)'
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -R desktop_workflow
```

Native Cocoa application startup requires access to the GUI session. Run these
tests through the execution environment's supported native GUI permission path;
do not infer a product failure from a sandbox's inability to create a window.
The test process exits after the suite and does not leave an app running.

## Disk-full test

`tests/diskfull_test.cpp` exercises the actual `cbcore` transaction code against a dedicated 2 MiB tmpfs. `packaging/test_diskfull.sh` creates the mount under `RUNNER_TEMP`, runs the helper as the ordinary CI user, and unmounts it in an exit trap. Only the mount/unmount commands use `sudo -n`. The helper has a 30-second deadline and a 5-second termination grace period.

The helper refuses non-Linux builds, noncanonical paths, symlink aliases, a nonempty directory, a directory that is not a distinct mount root, a non-tmpfs filesystem, a mount not owned by the current user with mode 0700, or a volume outside 1–4 MiB. Both Linux `statfs` and Qt `QStorageInfo` must recognize the exact mount. It allocates at most the guarded volume size plus a small block, using real `write(2)` calls; sparse files and injected I/O errors are not used. A fill must fail with Linux `ENOSPC` (errno 28) or the test fails.

The fixtures and application lock are isolated in an owned `QTemporaryDir` beside the mount. The helper verifies that this directory is on another filesystem. The cleanup trap removes only the now-empty directories it created using `rmdir`; failed unmounts or unexpected leftover files are preserved and reported. The test never accepts a source folder or an existing backup directory from a user.

Two scenarios run:

1. **Backup volume full.** Original and conflict fixtures live outside tmpfs. After observing native `ENOSPC`, the helper truncates its own filler to reserve 160 KiB. The receipt and 64 KiB original backup must complete; a 512 KiB conflict backup must fail at the real `QSaveFile` data write. The second backup must not appear as a committed file. The durable receipt must remain `backing_up` and History must expose it.
2. **Destination volume full.** Both source fixtures live on tmpfs and the backups live outside it. The helper again fills to native `ENOSPC`, then reserves 64 KiB. Both backups must be intact, while the 512 KiB atomic replacement must fail at its real `QSaveFile` data write. The receipt must remain `writing_destination`.

In both cases `execute()` must report failure, never `committed`. Original and conflict bytes, inode identity, size, owner/group, mode, link count, and nanosecond modification time must remain unchanged. The helper removes only its own filler, explicitly calls `undo()` with the exact returned receipt, and verifies an `undone` receipt and unchanged source witnesses. JSON lines record mount size, native errno, free bytes, the application error, durable stages, and each recovery result. Any missing assertion or non-ENOSPC fill is a nonzero exit; CI must preserve its normal step log.

This is a real Linux capacity failure test, not evidence of Windows/macOS disk-full behavior or of whole-transaction atomicity. It does not simulate power loss, physical media faults, inode exhaustion, quotas, or hostile concurrent writers. Other safety and crash tests cover their explicitly named cases. The helper's native fill records errno; Qt's returned error text is recorded independently, since diagnostic strings may differ by Qt version. The original backup and durable-stage assertions ensure an unrelated early failure cannot pass.

### Observed CI evidence

The Ubuntu 24.04 job for commit `bebe83e50f31002bdc2d29c4e414bd5b65e0761c` passed both scenarios on 2026-10-08. Its [public job log](https://github.com/rad1092/conflictbench/actions/runs/37718776547/job/113121421731) records a 2,097,152-byte tmpfs, native errno 28, zero available bytes before reserving headroom, and `diskfull_suite_passed` with two scenarios. Backup and destination failures retained `backing_up` and `writing_destination`, respectively; both recovered to `undone` with unchanged source witnesses. This records the disk-full step specifically: the overall job later failed its separate clean-container smoke because GLib was missing from the documented runtime dependency list.

### Build and CI integration

Inside the existing `if(BUILD_TESTING)` block, add a Linux-only helper target:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(diskfull_test tests/diskfull_test.cpp)
  target_link_libraries(diskfull_test PRIVATE cbcore)
endif()
```

Run the privileged mount fixture as a separate Linux CI step after the normal build, rather than silently requiring mount privileges in every `ctest` invocation:

```yaml
- name: Real disk-full transaction and recovery
  if: runner.os == 'Linux'
  shell: bash
  run: bash packaging/test_diskfull.sh "$PWD/build/diskfull_test"
```

On a dedicated local Linux test machine, set `RUNNER_TEMP` to an existing writable temporary directory and use the same command. Passwordless mount privileges are required; inability to mount is a failed validation, not a skipped pass. Do not run the helper against an existing user filesystem. Ordinary developers can continue running the regular core tests without `sudo`.
