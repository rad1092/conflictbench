# Building and release verification

ConflictBench is a C++17 / Qt Widgets application. The application has no Python
runtime dependency; the Python scripts below only assemble and verify releases.
Use CMake 3.24 or newer, a C++17 compiler, and shared Qt Core, Gui, Widgets,
Concurrent, and Test. CMake accepts Qt 6.8 or newer. Release packages pin Qt
**6.11.2**, the selected 6.11 patch release, to include its security and quality
fixes. See [Qt's release announcement](https://www.qt.io/blog/qt-6.11.2-released).
This is an exact dependency pin, not a claim to ship the newest Qt minor series.

## Build

Install Qt from its [official downloads](https://www.qt.io/download-qt-installer-oss)
or use your operating system's package manager for a development build.
`qt_prefix` means the architecture directory containing `bin/qmake`, not its
parent version directory.

macOS or Linux with Ninja:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$qt_prefix" -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 1
```

For Homebrew development on Apple Silicon, use
`-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase`. Official release builds target
Apple Silicon and macOS 13 or newer. CI runs on a macOS 15 Apple Silicon host;
that does not claim testing on every supported macOS version or Intel Macs.

Windows, from a Visual Studio 2022 developer shell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_PREFIX_PATH=$env:QT_ROOT_DIR" -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --parallel 1
```

Launch `build/ConflictBench.app` on macOS, `build/Release/ConflictBench.exe` on
Windows, or `build/ConflictBench` on Linux. `--smoke-test` exercises the GUI and
a synthetic commit/undo in temporary directories, then exits with a status code.
On headless Linux use `xvfb-run -a build/ConflictBench --smoke-test`. The built-in
demo uses self-created data and can be opened without choosing a personal folder.

## Packages

Use Python 3.12 or newer. The source download is verified against a committed
SHA-256; it is not built or executed by the packaging script.

```sh
python3 packaging/prepare_sources.py
python3 packaging/package.py --qt "$qt_prefix"
```

On Windows, substitute `python` for `python3`. Run the Linux package command
under Xvfb when no desktop display is available. For local Homebrew verification,
append `--development`; the resulting archive is explicitly excluded from
release assembly because Homebrew's dependency composition differs from the
official Qt binaries.

The package command installs to `package-work/`, adds runtime libraries, plugins,
licenses, and documentation, archives the result, extracts it under a path with
spaces and Korean characters, and runs the extracted program with development
Qt environment variables removed. Outputs in `dist/` include the archive,
SHA-256, and a manifest with the source commit, runtime module list, architecture,
and file hashes. The manifest outside the archive inventories the final signed
files; its smaller in-package counterpart records build identity.

| Package | Included runtime | System requirements and verification |
| --- | --- | --- |
| macOS ZIP | `.app`, Qt frameworks, Cocoa/offscreen, native style and basic image plugins | Apple Silicon; macOS 13+ target, CI on 15; ad-hoc signature check and extracted launch |
| Windows ZIP | App, Qt DLLs, Windows/offscreen, style/image plugins, app-local Microsoft VC143 runtime | x86_64 Windows; CI on Windows Server 2022; extracted launch |
| Linux tar.gz | App, Qtbase shared libraries, ICU 73.2, XCB/offscreen and basic image plugins | Ubuntu 24.04 x86_64; X11/XWayland; documented apt libraries; extracted launch in a clean Ubuntu container |

Linux is a distribution-specific portable folder, not a universal Linux binary.
The full apt command is in [packaging/linux-runtime.txt](../packaging/linux-runtime.txt).
OS graphics drivers, glibc, and system desktop libraries are not bundled.

macOS bundles are ad-hoc signed to permit local Apple Silicon loading. They are
**not Developer ID signed or notarized**. Windows executables are **not
Authenticode signed**. These packages may trigger unknown-publisher warnings.
No script changes system security settings. Verify the published checksum and
use the operating system's normal per-application opening controls.

## CI and publication

`.github/workflows/ci.yml` runs on every push and pull request. Qt binary archive
selection is explicit, GitHub actions are pinned to full commits, aqtinstall is
pinned to 3.3.0, and py7zr to 1.1.0. The inspected
[install-qt-action source](https://github.com/jurplel/install-qt-action/tree/a9c63c7c123f3069cff414e7e482d95dfa9d8125)
uses `aqt install-qt` to download Qt's public distribution; it does not require a
Qt account or use the commercial installer. Pip resolves some transitive build
tool dependencies, so the workflow is repeatable by declared versions but is not
a fully hermetic build.

Qt 6.11 changed the Windows archive layout. The Windows job uses the small [pinned direct downloader](QT-WINDOWS-INSTALL.md) for official Qtbase/Qttools archives; macOS/Linux continue through aqt. No account or commercial installer is used.

Each desktop job builds with at most two compiler processes, runs CTest, packages,
and tests an extracted application. The Linux job additionally launches the
archive in a clean container with only documented system dependencies. Logs,
packages, source notices, and secret-scan results are retained as CI artifacts.
The Gitleaks binary has a fixed version and independently checked SHA-256.
The single reviewed historical prose false positive has an exact fingerprint
exception, documented in [SECRET-SCAN-REVIEW](SECRET-SCAN-REVIEW.md).
Dependency checks enforce pinned actions, source hashes, and module scope, and
query the public OSV database for advisories mapped to the exact Qtbase and ICU
source commits. Results retain the query and check date. OSV's commit coverage
can be incomplete, including embedded third-party components, so no mapped
findings is not a substitute for checking current Qt security advisories.

To release, first run and review a green CI build for the exact intended commit.
Set the version in `CMakeLists.txt` and update the changelog before making the
version tag. Pushing `v<version>` reruns the full matrix. Only after all package
and security jobs pass does it prepare a **draft** GitHub release. The draft
contains three platform archives, full application source for that exact commit,
unmodified Qtbase corresponding source, manifests, and `SHA256SUMS`. Review the
draft and checksums before publishing. A failed platform prevents release
assembly. No signing credentials are requested or stored.

Release assets also preserve the small core/GUI logs, actual ENOSPC evidence,
redacted secret-scan result and dependency report, with a verification manifest
linking the exact workflow. This keeps the supporting evidence available after
temporary CI artifacts expire.

Test-generated folders and caches are ignored. Delete `build/`, `package-work/`,
`release-sources/`, and `dist/` after preserving the small reports and published
artifacts you need. Do not include personal conflict files in issues or tests.
