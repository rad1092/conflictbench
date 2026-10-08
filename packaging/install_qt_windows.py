#!/usr/bin/env python3
"""Install the two pinned official Qt Windows archives used by ConflictBench CI."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET


QT_VERSION = "6.11.2"
PACKAGE_VERSION = "6.11.2-0-202608131017"
PACKAGE = "qt.qt6.6112.win64_msvc2022_64"
REPOSITORY = (
    "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/"
    "qt6_6112/qt6_6112_msvc2022_64/"
)
XML_SHA256 = "47a07e2a22f453cb89028c3a0524f64467f1114cbf57936158784ebda2d7bdcc"
ARCHIVE_SUFFIX = "-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
# SHA-256 calculated on 2026-10-08 after matching Qt's published .sha1 files.
ARCHIVES = {
    "qtbase": {
        "size": 39618573,
        "sha1": "1fca9483183d426c1a3a984f1382382fa51f48de",
        "sha256": "fd984b7264361b4dd3fd2a417702ca1258e4086268f2ee6a69b9a393d9c3f6bb",
    },
    "qttools": {
        "size": 27033907,
        "sha1": "0a94da47fb97103660b5bfec7361ae51b05f7323",
        "sha256": "5f2b387a1f8055102b1388ff4ea743124ed6389d442f75c659728bb5c2a56946",
    },
}


class HTTPSOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise RuntimeError("Refusing a non-HTTPS Qt download redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(url, destination, maximum, sha256=None):
    """Bounded retry, size, socket timeout and elapsed time; no partial reuse."""
    opener = urllib.request.build_opener(HTTPSOnlyRedirect())
    for attempt in range(3):
        digest = hashlib.sha256()
        size = 0
        started = time.monotonic()
        try:
            with opener.open(url, timeout=30) as response, destination.open("wb") as output:
                while chunk := response.read(1024 * 1024):
                    size += len(chunk)
                    if size > maximum or time.monotonic() - started > 300:
                        raise RuntimeError("Qt download exceeded size or time limit")
                    output.write(chunk)
                    digest.update(chunk)
            if sha256 and digest.hexdigest() != sha256:
                raise RuntimeError(f"SHA-256 mismatch for {destination.name}")
            return size
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            destination.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(2 * (attempt + 1))


def get_archives(directory):
    metadata = directory / "Updates.xml"
    download(REPOSITORY + "Updates.xml", metadata, 256 * 1024, XML_SHA256)
    matches = [p for p in ET.parse(metadata).getroot().findall("PackageUpdate")
               if p.findtext("Name") == PACKAGE]
    if len(matches) != 1 or matches[0].findtext("Version") != PACKAGE_VERSION:
        raise RuntimeError("Official Qt metadata does not match the pinned package")
    available = {s.strip() for s in matches[0].findtext("DownloadableArchives", "").split(",")}
    result = []
    for module, pin in ARCHIVES.items():
        archive = module + ARCHIVE_SUFFIX
        if archive not in available:
            raise RuntimeError(f"{archive} is missing from official Qt metadata")
        name = PACKAGE_VERSION + archive
        url = REPOSITORY + PACKAGE + "/" + name
        published_hash = directory / (name + ".sha1")
        download(url + ".sha1", published_hash, 128)
        if published_hash.read_text(encoding="ascii").strip() != pin["sha1"]:
            raise RuntimeError(f"Qt's published SHA-1 changed for {module}")
        path = directory / name
        print(f"Downloading pinned {module} ({pin['size']} bytes)", flush=True)
        size = download(url, path, pin["size"], pin["sha256"])
        # This is an additional provenance check; SHA-256 is the security pin.
        with path.open("rb") as source:
            actual_sha1 = hashlib.file_digest(source, "sha1").hexdigest()
        if size != pin["size"] or actual_sha1 != pin["sha1"]:
            raise RuntimeError(f"Size or publisher SHA-1 mismatch for {module}")
        result.append(path)
        print(json.dumps({"module": module, "url": url, **pin}), flush=True)
    return result


def seven_zip():
    path = shutil.which("7z")
    if not path:
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "7-Zip/7z.exe"
        if candidate.is_file():
            path = str(candidate)
    if not path:
        raise RuntimeError("7-Zip is required (provided by GitHub windows-2022 runners)")
    return path


def extract(executable, archive, destination):
    listing = subprocess.run(
        [executable, "l", "-slt", "-ba", str(archive)], check=True,
        capture_output=True, text=True, errors="replace", timeout=60,
    ).stdout
    names = [line[7:] for line in listing.splitlines() if line.startswith("Path = ")]
    if not names or "Symbolic Link = " in listing or "Hard Link = " in listing:
        raise RuntimeError("Unexpected empty or linked Qt archive")
    for name in names:
        normalized = name.replace("\\", "/")
        if normalized.startswith("/") or ":" in normalized or ".." in PurePosixPath(normalized).parts:
            raise RuntimeError(f"Unsafe Qt archive path: {name!r}")
    subprocess.run(
        [executable, "x", "-y", "-bd", "-bso0", "-bsp0", f"-o{destination}", str(archive)],
        check=True, timeout=240,
    )
    print(f"Extracted {len(names)} entries from {archive.name}", flush=True)


def verify_install(prefix):
    required = ("bin/qmake.exe", "bin/windeployqt.exe", "bin/Qt6Core.dll",
                "plugins/platforms/qwindows.dll", "lib/cmake/Qt6/Qt6Config.cmake")
    for relative in required:
        if not (prefix / relative).is_file():
            raise RuntimeError(f"Qt installation is missing {relative}")
    result = subprocess.run([str(prefix / "bin/qmake.exe"), "-query"],
                            check=True, capture_output=True, text=True, timeout=30)
    values = dict(line.split(":", 1) for line in result.stdout.splitlines() if ":" in line)
    if values.get("QT_VERSION") != QT_VERSION:
        raise RuntimeError("qmake reports an unexpected Qt version")
    if Path(values.get("QT_INSTALL_PREFIX", "")).resolve() != prefix.resolve():
        raise RuntimeError("qmake did not relocate to the installation prefix")
    if Path(values.get("QT_INSTALL_PLUGINS", "")).resolve() != (prefix / "plugins").resolve():
        raise RuntimeError("qmake reports an unexpected plugin path")
    print(result.stdout, end="", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="parent of 6.11.2/msvc2022_64; existing kit rejected")
    parser.add_argument("--verify-downloads", action="store_true", help="verify archives only; works on any OS")
    args = parser.parse_args()
    if not args.verify_downloads and (os.name != "nt" or not args.output):
        parser.error("installation requires Windows and --output (or use --verify-downloads)")
    if args.verify_downloads:
        with tempfile.TemporaryDirectory(prefix="conflictbench-qt-verify-") as temporary:
            get_archives(Path(temporary))
        print("Official Qt Windows archive pins verified; no SDK installed.")
        return

    prefix = args.output.resolve() / QT_VERSION / "msvc2022_64"
    if any(character in str(prefix) for character in "\r\n"):
        raise RuntimeError("Newlines are not allowed in the installation path")
    if prefix.exists() or prefix.is_symlink():
        raise RuntimeError(f"Refusing to overwrite an existing Qt kit: {prefix}")
    executable = seven_zip()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".qt-stage-", dir=prefix.parent) as temporary:
        staging = Path(temporary)
        kit = staging / "kit"
        kit.mkdir()
        for archive in get_archives(staging):
            extract(executable, archive, kit)
        (kit / "bin/qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="utf-8")
        verify_install(kit)
        kit.rename(prefix)
    verify_install(prefix)
    (prefix / "conflictbench-qt-provenance.json").write_text(
        json.dumps({"repository": REPOSITORY, "package": PACKAGE,
                    "package_version": PACKAGE_VERSION, "metadata_sha256": XML_SHA256,
                    "archives": ARCHIVES}, indent=2) + "\n", encoding="utf-8")
    if os.environ.get("GITHUB_ENV") and os.environ.get("GITHUB_PATH"):
        with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as environment:
            environment.write(f"QT_ROOT_DIR={prefix}\nQT_PLUGIN_PATH={prefix / 'plugins'}\n")
        with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as path_file:
            path_file.write(str(prefix / "bin") + "\n")
    print(f"Installed verified Qt {QT_VERSION}: {prefix}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        print(f"Qt installation failed: {error}", file=sys.stderr)
        sys.exit(1)
