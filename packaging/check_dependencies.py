#!/usr/bin/env python3
"""Fail closed on changed release dependencies; produce a reviewable inventory.

This checks versions and provenance, not the absence of runtime vulnerabilities.
Review Qt advisories before each release; see docs/LICENSING.md.
"""
import hashlib
import json
from pathlib import Path
import re
import sys
import urllib.request
from datetime import datetime, timezone
from prepare_sources import QT_VERSION, QT_SOURCE_URL, QT_SOURCE_SHA256

root = Path(__file__).resolve().parents[1]
actions = []
for workflow in (root / ".github/workflows").glob("*.yml"):
    for line in workflow.read_text(encoding="utf-8").splitlines():
        match = re.search(r"uses:\s+(\S+)", line)
        if not match:
            continue
        dependency = match.group(1)
        if not re.fullmatch(r"[\w./-]+@[0-9a-f]{40}", dependency):
            raise RuntimeError(f"Unpinned action: {dependency}")
        actions.append(dependency)
notices = {}
for path in sorted((root / "packaging/licenses").iterdir()):
    if path.is_file():
        notices[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
expected = {
    "ICU-73.2-LICENSE": "f3005e195ff74d8812cc1f182a1c446fab678d70a10e3dada497585befee5416",
    "Microsoft-Visual-C-Runtime-2015-2022-License.docx": "f1e3d56ceb2ad68aae0711b910375009e651ac5530fa0760f0dea6e81e54fae1",
}
for name, digest in expected.items():
    if notices.get(name) != digest:
        raise RuntimeError(f"Upstream notice changed: {name}")
report = {"qt_version": QT_VERSION, "qt_source_url": QT_SOURCE_URL,
          "qt_source_sha256": QT_SOURCE_SHA256, "runtime_modules": "qtbase only; enforced by package.py",
          "linux_icu": "73.2 from official Qt binary distribution",
          "actions": sorted(set(actions)), "notice_sha256": notices,
          "limitations": "Inventory/provenance checks do not prove absence of vulnerabilities."}
if "--online" in sys.argv:
    report["osv_checked_at"] = datetime.now(timezone.utc).isoformat()
    report["osv"] = []
    for name, commit in {
        "qtbase-6.11.2": "ef55f427f2c8b410d34f8a7681020a3000cf6866",
        "icu-73.2": "680f521746a3bd6a86f25f25ee50a62d88b489cf",
    }.items():
        query = {"commit": commit}
        request = urllib.request.Request("https://api.osv.dev/v1/query", data=json.dumps(query).encode(), headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=45) as response:
            data = json.load(response)
        findings = [entry for entry in data.get("vulns", []) if not entry.get("withdrawn")]
        report["osv"].append({"component": name, "query": query, "findings": findings})
        if findings or data.get("next_page_token"):
            (root / "dependency-evidence.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            raise RuntimeError(f"Dependency advisories need review: {name}")
    report["limitations"] += " OSV commit coverage can be incomplete, especially for Qt and embedded third-party code; no mapped findings is not proof of no vulnerabilities."
(root / "dependency-evidence.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report, indent=2))
