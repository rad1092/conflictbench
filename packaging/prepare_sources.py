#!/usr/bin/env python3
"""Fetch exact, checksum-pinned Qt source; extract notices without executing it."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import urllib.request

QT_VERSION = "6.11.2"
QT_SOURCE_NAME = f"qtbase-everywhere-src-{QT_VERSION}.tar.xz"
QT_SOURCE_SHA256 = "5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22"
QT_SOURCE_URL = f"https://download.qt.io/archive/qt/6.11/{QT_VERSION}/submodules/{QT_SOURCE_NAME}"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def prepare(destination):
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / QT_SOURCE_NAME
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        with urllib.request.urlopen(QT_SOURCE_URL, timeout=120) as response, partial.open("wb") as output:
            while block := response.read(1024 * 1024):
                output.write(block)
        partial.replace(archive)
    if sha256(archive) != QT_SOURCE_SHA256:
        raise RuntimeError(f"Qt source checksum mismatch: {archive}")
    notices = destination / "qt-notices"
    notices.mkdir(exist_ok=True)
    attributions = []
    with tarfile.open(archive, "r:xz") as source:
        members = {member.name: member for member in source if member.isfile()}
        wanted = set()
        for name, member in members.items():
            short = PurePosixPath(name).name
            if re.search(r"license|licence|copying|copyright|notice|readme|authors", short, re.I):
                wanted.add(name)
            if short == "qt_attribution.json":
                wanted.add(name)
                # Upstream attribution text can contain literal newlines in strings.
                data = json.loads(source.extractfile(member).read(), strict=False)
                entries = data if isinstance(data, list) else [data]
                for entry in entries:
                    attributions.append({"source": name, **entry})
                    license_file = entry.get("LicenseFile", "")
                    if isinstance(license_file, str) and license_file:
                        import posixpath
                        candidate = posixpath.normpath(str(PurePosixPath(name).parent / license_file))
                        if candidate in members:
                            wanted.add(candidate)
        for name in sorted(wanted):
            relative = PurePosixPath(name)
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError("Unsafe path in Qt source")
            target = notices.joinpath(*relative.parts[1:])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.extractfile(members[name]).read())
    (notices / "ATTRIBUTIONS.json").write_text(json.dumps(attributions, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (destination / "QT-SOURCE.json").write_text(json.dumps({
        "module": "qtbase", "version": QT_VERSION, "url": QT_SOURCE_URL,
        "filename": QT_SOURCE_NAME, "sha256": QT_SOURCE_SHA256,
        "modifications": "None; shared libraries from official Qt binary archives.",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {QT_SOURCE_NAME}; {len(wanted)} notices and {len(attributions)} attribution records")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("release-sources"))
    prepare(parser.parse_args().output)
