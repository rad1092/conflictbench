# Native macOS workflow evidence

On 2026-10-08, the complete `gui_tests` suite passed using Qt's native Cocoa
platform plugin on macOS 27.0.1 / Apple Silicon, Qt 6.11.2 (shared libraries),
Apple LLVM 21.0.0. All files were generated demo fixtures or isolated temporary
test files. No personal folder, live Syncthing instance, or desktop capture was
used.

| Run | Result | Duration |
| --- | --- | --- |
| Native Cocoa, default scale | 12 passed, 0 failed, 0 skipped | 3.220 s |
| Native Cocoa, `QT_SCALE_FACTOR=2` | 12 passed, 0 failed, 0 skipped | 3.654 s |
| Offscreen regression | 12 passed, 0 failed, 0 skipped | 0.705 s |

The count includes QtTest initialization and cleanup. These are working-tree
checks based on `e2b76e148443dd878efa4ae5d3d6e365413924e0` plus the GUI harness,
UI, and core fixes under review; the release workflow must verify the final
commit separately. The final local run was at 02:35 UTC with source SHA-256:

| Source | SHA-256 |
| --- | --- |
| `src/core.cpp` | `33a0e91233d095f1f198013ae96fb0036f27f6fd9c23d557e9deed46e3c080c7` |
| `src/workbench.cpp` | `3c6ab122d4dfe5cc4240d26f40fa0ea4916d775ab04471f40f2c661db8964444` |
| `tests/gui_tests.cpp` | `8bc4f992fdca55be10936f2fe67fd683daae2d0f2959fb60bf80976353094dc1` |

The accompanying screenshot files preserve the result of this run, not a claim
about later builds.

The suite exercises keyboard demo activation; Korean Unicode text and filenames;
decoded image and binary previews; all three explicit decisions; acknowledgment
gating and cancel-without-writes; receipt visibility; same-size source changes
before review and while review is open; a new backup selection after leaving
the demo; and external executable launch failure. Every decision is checked
against resulting file bytes and the receipt; rollback of each receipt is
checked using the core API. See [the full coverage description](GUI-TESTS.md).

A dedicated regression selects the last index in the scan vector, commits it,
then verifies that the shortened scan has a valid new selection. It compares
every remaining directory entry and file hash, checks the receipt's original
path, and checks that review remains enabled. This covers the stale selection
index found when Linux sorted the Korean filename last; it does not rely on an
OS-specific filename sort order.

## Captures

The optional `CB_GUI_CAPTURE_DIR` environment variable saves only a synthetic
application widget with `QWidget::grab()`. It never captures the desktop or other
applications. A failed requested capture fails the test. Default test runs save
no images. Each capture logs its pixel size and device pixel ratio.

- [Text and Korean filename](screenshots/cocoa-text.png): 1180 × 820, DPR 1.
- [Image versions](screenshots/cocoa-image.png): 1180 × 820, DPR 1.
- [Binary explicit-choice guidance](screenshots/cocoa-binary.png): 1180 × 820, DPR 1.
- [Plan and initially disabled commit](screenshots/cocoa-plan.png): 720 × 540, DPR 1.
- [Committed receipt and disabled undo](screenshots/cocoa-history.png): 850 × 520, DPR 1.
- [Text at Qt scale factor 2](screenshots/cocoa-text-scale2.png): 1920 × 1200, DPR 2.

All six refreshed captures were visually inspected. Both image versions now fit
inside their preview panes; binary guidance wraps and is fully readable; long
metadata filenames are deliberately elided with the full value in a tooltip.
Long plan paths remain horizontally scrollable. The scale-2 run
reported Qt window-placement warnings when a requested logical window exceeded
the available screen area, but all workflow assertions passed. This checks
Qt's scaled rendering and interaction, not moving a physical window between
monitors.

## Picker harness repair

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

## Reproduction

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
