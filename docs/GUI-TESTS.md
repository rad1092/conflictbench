# Desktop workflow verification

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
