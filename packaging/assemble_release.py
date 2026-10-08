#!/usr/bin/env python3
"""Collect tested archives plus complete application/Qt source and checksums."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from prepare_sources import QT_SOURCE_NAME, QT_SOURCE_SHA256, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("dist/release"))
    args = parser.parse_args()
    version = re.search(r"project\(ConflictBench\s+VERSION\s+(\d+\.\d+\.\d+)", Path("CMakeLists.txt").read_text()).group(1)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    args.output.mkdir(parents=True, exist_ok=True)
    manifests = list(args.artifacts.rglob("ConflictBench-*-manifest.json"))
    systems = set()
    for path in manifests:
        data = json.loads(path.read_text(encoding="utf-8"))
        if data["commit"] != commit or data["version"] != version or data["development_only"]:
            raise RuntimeError(f"Release manifest mismatch: {path}")
        systems.add(data["os"])
    if systems != {"Darwin", "Windows", "Linux"}:
        raise RuntimeError(f"Expected three tested systems, found {systems}")
    for path in args.artifacts.rglob("*"):
        if path.is_file() and path.name.startswith("ConflictBench-") and path.name.endswith((".zip", ".tar.gz", "-manifest.json")):
            shutil.copy2(path, args.output / path.name)
    qt_source = next(args.artifacts.rglob(QT_SOURCE_NAME))
    if sha256(qt_source) != QT_SOURCE_SHA256:
        raise RuntimeError("Qt corresponding source checksum mismatch")
    shutil.copy2(qt_source, args.output / qt_source.name)
    subprocess.run(["git", "archive", "--format=tar.gz", f"--prefix=ConflictBench-{version}/", f"--output={args.output / ('ConflictBench-' + version + '-source.tar.gz')}", "HEAD"], check=True)
    checksums = "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(args.output.iterdir()) if path.is_file() and path.name != "SHA256SUMS")
    (args.output / "SHA256SUMS").write_text(checksums, encoding="utf-8")
    print(f"Release assets prepared for {commit}, version {version}")


if __name__ == "__main__":
    main()
