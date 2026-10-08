# Pinned Windows Qt installation

Qt 6.11 changed the Windows online repository into architecture-specific
subdirectories. aqtinstall 3.3.0 requests the older layout and fails before
installation ([upstream issue #1007](https://github.com/miurahr/aqtinstall/issues/1007)).
ConflictBench uses `packaging/install_qt_windows.py` for its Windows CI job.
This narrowly scoped Python 3.11+ script downloads the official Qt 6.11.2
MSVC 2022 x64 `qtbase` and `qttools` archives; it is not a general Qt installer.
It needs no Python packages, Qt account, credentials, administrator rights,
or registry changes. GitHub's Windows runner supplies 7-Zip.

```yaml
- name: Install pinned official Windows Qt archives
  if: runner.os == 'Windows'
  shell: pwsh
  run: python packaging/install_qt_windows.py --output "${{ runner.temp }}/conflictbench-qt"
```

Keep the existing install-qt action limited to non-Windows runners. After
installation the script appends `QT_ROOT_DIR` and `QT_PLUGIN_PATH` to
`GITHUB_ENV`, and the SDK `bin` directory to `GITHUB_PATH`. It does so only
after executing the installed `qmake.exe` and confirming its version,
prefix, and plugin path. Outside GitHub Actions it prints the prefix but
does not change the caller's environment. The destination kit must not exist.

## Provenance and pins

The source is Qt's [official architecture-specific repository](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112_msvc2022_64/).
Package `qt.qt6.6112.win64_msvc2022_64`, version
`6.11.2-0-202608131017`, lists both archives in `Updates.xml`.
Qt's download endpoint can redirect to a Qt-selected distribution mirror;
all redirects must use HTTPS, and the downloaded bytes must match the
committed SHA-256 pins. No installer-supplied scripts are executed.

The 2026-10-08 audit downloaded both archives through Qt's endpoint, matched
each official `.sha1` sidecar, and calculated these SHA-256 hashes:

| Archive | Bytes | SHA-256 |
| --- | ---: | --- |
| qtbase | 39,618,573 | `fd984b7264361b4dd3fd2a417702ca1258e4086268f2ee6a69b9a393d9c3f6bb` |
| qttools | 27,033,907 | `5f2b387a1f8055102b1388ff4ea743124ed6389d442f75c659728bb5c2a56946` |

The full names are the package version followed by the module and
`-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z`.
`Updates.xml` is also pinned to SHA-256
`47a07e2a22f453cb89028c3a0524f64467f1114cbf57936158784ebda2d7bdcc`.
The official SHA-1 values are additional provenance checks; SHA-256 is the
enforced security pin. Changed metadata or archive bytes cause failure and
require a reviewed pin update. The installed kit contains
`conflictbench-qt-provenance.json` with these values.

## Extraction and relocation

Inspection on macOS using libarchive found 4,731 qtbase and 633 qttools
entries, with flat `bin`, `lib`, `include`, `mkspecs`, and `plugins` roots.
They are extracted into a newly created staging directory, matching the
target path specified by Qt's metadata. Paths containing parent traversal,
absolute paths, drive/stream separators, or link entries are rejected
before extraction. Downloads enforce exact expected size and hashes, three
bounded network attempts, and socket and elapsed-time limits. Extraction
and executable verification also have timeouts.

The archive's CMake configuration is relocatable. A small
`bin/qt.conf` containing `[Paths]` and `Prefix=..` makes the installed tools
resolve the selected SDK path. The script verifies `qmake -query` both in
staging and after moving the kit into place. It never binary-patches Qt.
If final verification fails, no GitHub variables are exported; remove that
new failed kit explicitly before retrying. Ordinary staging/download data
is removed on exit. A process kill can leave a `.qt-stage-*` directory in
the chosen output directory, suitable for manual cleanup.

## Validation boundaries

On any platform, verify the official metadata, sidecars, and full archive
pins without extraction or environment changes:

```sh
python3 packaging/install_qt_windows.py --verify-downloads
```

On 2026-10-08 that exact command completed successfully on macOS, re-fetching
the official metadata and both full archives. Local synthetic checks also
confirmed refusal of oversized/corrupt downloads and traversal, absolute,
drive, stream, and link archive entries. Syntax, `--help`, and refusing an
installation on non-Windows hosts passed. Downloaded audit copies were
removed after recording their counts and hashes above.

macOS archive inspection and hash checks do not establish Windows runtime
compatibility. The Windows CI job must execute `qmake`, configure and build
ConflictBench, run its safety/UI tests, and package and smoke-test the
extracted application. Only that actual Windows job establishes the SDK
installation works for this release. Licensing and redistribution remain
covered by [LICENSING.md](LICENSING.md) and the exact corresponding source
archives assembled by the release workflow.
