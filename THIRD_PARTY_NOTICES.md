# Third-party notices

ConflictBench uses **Qt 6.11.2**, copyright The Qt Company Ltd. and other
contributors, under the GNU Lesser General Public License, version 3. The
application itself is GPL-3.0-only. You may replace or rebuild its shared Qt
libraries. See [docs/LICENSING.md](docs/LICENSING.md) for source access and
replacement instructions. Copies of the GNU GPL and LGPL are included in
`packaging/licenses/` and with every packaged binary.

Release packages contain only Qtbase runtime modules and selected platform,
style, and basic image plugins. Qtbase includes separately licensed third-party
code, including font, text, image, and compression components. Its exact upstream
attribution records and copyright/license files are copied into
`licenses/qtbase/` beside the packaged documentation. The manifest identifies
runtime libraries actually present. The full, unmodified Qtbase 6.11.2 source is
published alongside every release binary. Original terms govern each component.

Linux packages also include **ICU 73.2**, copyright Unicode, Inc. and other
contributors, from Qt's official ICU binary archive. Its complete upstream
license/third-party notice is `packaging/licenses/ICU-73.2-LICENSE`.
Source: <https://github.com/unicode-org/icu/tree/release-73-2>.

Windows packages include the unmodified **Microsoft Visual C++ 2015–2022
runtime**, copyright Microsoft Corporation, as app-local VC143 release DLLs.
The Microsoft runtime retains its own license; it is not relicensed under GPL.
The unmodified official terms are included as
`packaging/licenses/Microsoft-Visual-C-Runtime-2015-2022-License.docx`.
Source and redistribution terms:
<https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/>,
<https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution>.

Qt's small Windows **QtEntryPoint** startup library is BSD-3-Clause; its notice
is included with the Qtbase source and extracted notices. System libraries
provided by macOS, Windows, or the Linux distribution retain their original
licenses. CI tools are not shipped as application dependencies.

Qt and Syncthing names are used to identify compatibility. ConflictBench is an
independent project and is not an official Syncthing or Qt product.
