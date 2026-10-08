# Packaging validation evidence

Development-stage checks performed on 2026-10-08, before the initial repository
commit. These are supporting checks; the final release must pass CI at its exact
tagged commit.

- macOS Apple Silicon, Homebrew Qtbase 6.11.2: built application installed into
  a relocatable `.app`, selected Cocoa/offscreen/style/image plugins deployed,
  and `codesign --verify --deep --strict` passed for the ad-hoc signed bundle.
- The ZIP was extracted to a temporary path containing spaces and Korean text.
  Development Qt search paths were removed. Its native Cocoa `--smoke-test`
  printed `PASS desktop synthetic scan/preview/commit/undo` and exited zero.
- Development ZIP SHA-256:
  `72fd1391f2ccafbb9e46f6b5bea634e25f27942152284e5ecd795b5359826d13`.
  This Homebrew artifact is marked development-only and is rejected by the
  release assembler; public packages use official Qt distribution binaries.
- Qtbase 6.11.2 corresponding source SHA-256 matched Qt's published value.
  The notice extraction produced 269 files and 86 attribution records.
- Official Qt's Linux ICU archive was inspected: it contains ICU 73.2. The
  package script enforces this version and includes the exact upstream notice.
- Workflow YAML parsing, Python syntax compilation, shell syntax, dependency
  pins, and license-checksum validation passed.
- OSV public commit queries for Qtbase 6.11.2 and ICU 73.2 returned no mapped
  advisories. Exact queries and coverage limitations are emitted by
  `check_dependencies.py --online`; this is not a claim of complete CVE coverage.

The first native GUI launch inside the restricted shell sandbox failed in Cocoa
initialization. Re-running the same synthetic package check with authorized
desktop access passed. No personal folder was scanned or mutated.

Windows and Linux archive execution is intentionally a required CI check,
not inferred from the Mac result. The Linux check includes a clean Ubuntu 24.04
container with only documented desktop packages. Public release status must be
read from the workflow run, not this development evidence file.
