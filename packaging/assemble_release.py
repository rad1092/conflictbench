#!/usr/bin/env python3
"""Collect tested archives plus complete application/Qt source and checksums."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
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
    # Preserve small safety/installed-test evidence beyond CI artifact retention.
    evidence_files = {}
    for system in ("macos-15", "windows-2022", "ubuntu-24.04"):
        logs = list((args.artifacts / f"test-log-{system}").rglob("LastTest.log"))
        if len(logs) != 1:
            raise RuntimeError(f"Expected one test log for {system}, found {len(logs)}")
        evidence_files[f"{system}-LastTest.log"] = logs[0]
    for name in ("gitleaks.json", "dependency-evidence.json", "diskfull-evidence.jsonl"):
        matches = list(args.artifacts.rglob(name))
        if len(matches) != 1:
            raise RuntimeError(f"Expected one {name}, found {len(matches)}")
        evidence_files[name] = matches[0]
    if json.loads(evidence_files["gitleaks.json"].read_text(encoding="utf-8")):
        raise RuntimeError("Secret-scan report has unresolved findings")
    evidence_manifest = {
        "commit": commit, "version": version,
        "workflow_url": f"https://github.com/{os.environ.get('GITHUB_REPOSITORY', 'rad1092/conflictbench')}/actions/runs/{os.environ.get('GITHUB_RUN_ID', 'local')}",
        "files": {name: sha256(path) for name, path in evidence_files.items()},
        "scope": "Synthetic core/GUI tests, actual Linux ENOSPC, secret scan and mapped dependency advisories. Installed archive smoke results are in the linked workflow. These checks are not a universal safety guarantee."
    }
    evidence_path = args.output / f"ConflictBench-{version}-verification.json"
    evidence_path.write_text(json.dumps(evidence_manifest, indent=2) + "\n", encoding="utf-8")
    with tarfile.open(args.output / f"ConflictBench-{version}-verification-logs.tar.gz", "w:gz") as archive:
        for name, path in evidence_files.items():
            archive.add(path, arcname=name)
    checksums = "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(args.output.iterdir()) if path.is_file() and path.name != "SHA256SUMS")
    (args.output / "SHA256SUMS").write_text(checksums, encoding="utf-8")
    print(f"Release assets prepared for {commit}, version {version}")


if __name__ == "__main__":
    main()
