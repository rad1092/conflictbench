# Licensing and reproducible replacement of Qt

ConflictBench's application source is **GPL-3.0-only**; see [LICENSE](../LICENSE).
Copyright (C) 2026 ConflictBench contributors. Contributions are accepted under
the same license. The public source and build scripts accompany release binaries.

The application dynamically links Qt. Qt is copyright The Qt Company Ltd. and
other contributors. This distribution uses Qtbase under **LGPL-3.0-only**, with
the separate permissive licenses of its included third-party components. No
commercial Qt license is needed for this distribution model. Qt's alternative
licenses do not mean every Qt module has the same choices: the package script
rejects modules outside its explicit Qtbase allowlist. See
[Qt licensing](https://doc.qt.io/qt-6.11/licensing.html) and the
[third-party inventory](https://doc.qt.io/qt-6.11/licenses-used-in-qt.html).

| Component | Role | Distribution terms |
| --- | --- | --- |
| Qt Core, Gui, Widgets, Concurrent | Application libraries | LGPL-3.0-only option; dynamically linked |
| Qt DBus, OpenGL, PrintSupport, XcbQpa when present | Platform/plugin support from Qtbase | LGPL-3.0-only option; dynamically linked |
| Qtbase Cocoa/Windows/XCB/offscreen, styles, JPEG/GIF/ICO plugins | Desktop and basic image support | Qtbase licenses plus their third-party notices |
| QtEntryPoint on Windows | Small startup library linked by Qt | BSD-3-Clause, included in Qtbase source/notices |
| Embedded Qtbase dependencies | Text, fonts, images, compression and related functions | Exact attribution records and license files extracted from Qtbase 6.11.2 |
| ICU 73.2 on Linux | Unicode support required by official Qt binaries | Unicode/ICU permissive license and upstream third-party notices |
| Microsoft VC143 runtime on Windows | Compiler support DLLs | Unmodified Microsoft distributable code; separate runtime license |
| macOS/Windows/Linux system libraries | OS services | Supplied by the operating system; not relicensed by this project |

Qt Test and Qt deployment/build tools are development dependencies and are not
application runtime payloads. Qt tool licensing is documented in Qt's licensing
page, including the Qt GPL exception. No QtSvg, QtWebEngine, QML, or Qt Network
module is required by the application. The binary's manifest is authoritative for
the modules actually shipped on its platform.

## What is included with a binary

`share/conflictbench/` (inside `Contents/Resources/conflictbench/` on macOS)
contains the application GPL, this notice, GNU LGPL/GPL texts, Qt source identity,
and copied Qtbase copyright, license, and attribution records. Qtbase attribution
records cover a superset of optional features: inclusion of a notice alone does
not establish that a particular third-party component is linked in that binary.

The release provides `qtbase-everywhere-src-6.11.2.tar.xz` at the same download
location as the application. It is the exact unmodified upstream Qtbase source,
including its third-party code and build machinery, verified against Qt's
[published SHA-256](https://download.qt.io/archive/qt/6.11/6.11.2/submodules/qtbase-everywhere-src-6.11.2.tar.xz.sha256):

```text
5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22
```

The Qt shared binaries come from Qt's official online package archives and have
no source modifications by ConflictBench. Deployment tools adjust load paths;
macOS packaging adds an ad-hoc signature. `packaging/prepare_sources.py` verifies
and preserves this source archive. Release assembly refuses to omit it.

Windows packages copy only release DLLs from Visual Studio's
`VC/Redist/MSVC/<version>/x64/Microsoft.VC143.CRT`, never debug redistributables.
Microsoft permits redistribution of this code subject to its license terms;
see its [distributable list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
and [runtime terms](https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/).
An unmodified copy of those runtime terms is included as a DOCX. The Microsoft
runtime is not covered by the application's GPL and its copyright notices remain.

## Replacing or rebuilding Qt

You may modify the application or Qt, rebuild them, and debug changes, including
reverse engineering needed to debug changes to LGPL-covered libraries. No
application license or technical activation mechanism prevents this.

To build a modified Qtbase, extract the corresponding source, install its
documented build prerequisites, and run its `configure` script in a separate
build directory with `-shared -release -nomake examples -nomake tests` and your
chosen `-prefix`. Build and install with CMake. The source's own `configure -help`
and [Qt source build guide](https://doc.qt.io/qt-6/build-sources.html) describe
platform prerequisites and configuration options.

Then rebuild ConflictBench following [BUILDING.md](BUILDING.md), with
`CMAKE_PREFIX_PATH` pointing at your replacement Qt prefix. This is the supported
way to change Qt versions, build options, or ABI. For an interface-compatible
modified Qt build, you may also replace matching shared libraries and plugins:

- macOS: `ConflictBench.app/Contents/Frameworks` and `Contents/PlugIns`.
- Windows: Qt DLLs and plugin subdirectories beside `bin/ConflictBench.exe`.
- Linux: `lib/` and `plugins/`; the launcher sets only this process's library path.

Keep module versions, architecture, plugin ABI, and C++ runtime compatible.
Replacing files in a macOS bundle invalidates its ad-hoc signature. Re-sign your
own modified copy with `codesign --force --deep --sign - ConflictBench.app`.
No publisher certificate, account, server, or private key is required. Rebuild
from source if your operating system rejects a changed binary.

## Release maintenance

Review the [Qt security notices](https://www.qt.io/blog/tag/security), runtime
module inventory, source version, all dependency notices, and package smoke
results before each release. Bumping Qt requires updating the source version/hash,
binary archive selection, and re-running all platform tests. A dependency pin or
clean Gitleaks report is not a claim that dependencies have no vulnerabilities.
If new modules or externally bundled libraries appear, release packaging must
stop until their licenses and corresponding sources are accounted for.
