#!/usr/bin/env python3
"""Exercise reviewed, commit-pinned upstream methods on disposable synthetic data.

No upstream source is vendored. Supply deconflicter.py downloaded from the URL
in RESEARCH.md. Its SHA-256 must match before AST extraction or execution.
GUI callbacks are stubbed; the actual scan/action method bodies are unchanged.
"""

import ast
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import tempfile
from types import SimpleNamespace


COMMIT = "47438c05f7ad949b8062cdba2324c53d23efb638"
SOURCE_SHA256 = "e89de4fcb7fb207c53d9193dfcc8a03d81499523c388e9f9b8a924fa6035dc54"


class ListStub:
    def __init__(self):
        self.items = []

    def clear(self):
        self.items.clear()

    def addItem(self, item):
        self.items.append(item)

    def currentRow(self):
        return 0

    def count(self):
        return 0


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    source = Path(sys.argv[1]).read_bytes()
    if digest(source) != SOURCE_SHA256:
        raise SystemExit("Refusing source whose SHA-256 does not match the reviewed commit")
    parsed = ast.parse(source)
    resolver = next(n for n in parsed.body if isinstance(n, ast.ClassDef) and n.name == "ConflictResolver")
    methods = [n for n in resolver.body if isinstance(n, ast.FunctionDef) and n.name in {
        "scan_conflicts", "accept_version", "accept_and_cleanup"
    }]
    messages = []
    message_box = SimpleNamespace(
        information=lambda *args: messages.append("information"),
        warning=lambda *args: messages.append("warning"),
    )
    namespace = {"os": os, "QMessageBox": message_box}
    exec(compile(ast.Module(body=methods, type_ignores=[]), "pinned-upstream-methods", "exec"), namespace)
    results = []

    def context(root, base, conflict, editor_text=None):
        return SimpleNamespace(
            root_dir=str(root), conflict_map={str(base): [str(conflict)]},
            base_editor=SimpleNamespace(text=lambda: editor_text) if editor_text is not None else None,
            list_widget=ListStub(), scan_conflicts=lambda: None, clear_views=lambda: None,
        )

    def fixture(parent, name):
        root = parent / name
        root.mkdir()
        base = root / "report.txt"
        conflict = root / "report.sync-conflict-20260101-120000-ABCDEFG.txt"
        base.write_bytes(b"AAAA")
        conflict.write_bytes(b"BBBB")
        return root, base, conflict

    def same_stat_mutation(file, contents):
        before = file.stat()
        file.write_bytes(contents)
        os.utime(file, ns=(before.st_atime_ns, before.st_mtime_ns))
        assert file.stat().st_size == before.st_size
        assert file.stat().st_mtime_ns == before.st_mtime_ns

    with tempfile.TemporaryDirectory(prefix="conflictbench-comparison-") as temporary:
        parent = Path(temporary)
        root, base, conflict = fixture(parent, "changed-source")
        reviewed = conflict.read_bytes()
        same_stat_mutation(conflict, b"CCCC")
        namespace["accept_version"](context(root, base, conflict), str(conflict), str(base))
        assert base.read_bytes() == b"CCCC"
        results.append({"scenario": "source_changed_same_size_and_mtime_after_review",
                        "reviewed_sha256": digest(reviewed), "written_sha256": digest(base.read_bytes()),
                        "result": "unreviewed bytes written; no rejection"})

        root, base, conflict = fixture(parent, "changed-destination")
        same_stat_mutation(base, b"DDDD")
        namespace["accept_version"](context(root, base, conflict), str(conflict), str(base))
        assert base.read_bytes() == b"BBBB"
        results.append({"scenario": "destination_changed_same_size_and_mtime_after_review",
                        "result": "newer destination bytes overwritten; no rejection"})

        root, base, conflict = fixture(parent, "stale-editor")
        editor_text = base.read_text()
        same_stat_mutation(base, b"DDDD")
        namespace["accept_and_cleanup"](context(root, base, conflict, editor_text), str(base))
        assert base.read_bytes() == b"AAAA" and not conflict.exists()
        assert sorted(x.name for x in root.iterdir()) == ["report.txt"]
        results.append({"scenario": "accept_base_after_same_size_destination_change",
                        "result": "stale editor bytes restored; conflict deleted; no backup created"})

        root, base, conflict = fixture(parent, "disappeared-source")
        conflict.unlink()
        try:
            namespace["accept_version"](context(root, base, conflict), str(conflict), str(base))
        except FileNotFoundError:
            pass
        else:
            raise AssertionError("Expected missing-source error")
        assert base.read_bytes() == b""
        results.append({"scenario": "source_disappears_before_accept_version",
                        "result": "FileNotFoundError after destination truncated to zero bytes"})

        root, base, conflict = fixture(parent, "binary-base")
        binary = b"\xff\x00\xfe\x01"
        base.write_bytes(binary)
        # Same read/split/join path used by load_views and DiffScintilla.
        displayed_text = "\n".join(base.read_text(encoding="utf-8", errors="replace").splitlines())
        namespace["accept_and_cleanup"](context(root, base, conflict, displayed_text), str(base))
        assert base.read_bytes() != binary and not conflict.exists()
        results.append({"scenario": "accept_binary_base_without_editor_changes",
                        "before_hex": binary.hex(), "after_hex": base.read_bytes().hex(),
                        "result": "UTF-8 replacement characters written; binary bytes changed"})

        root = parent / "extensionless"
        root.mkdir()
        base = root / "README"
        conflict = root / "README.sync-conflict-20260101-120000-ABCDEFG"
        base.write_bytes(b"AAAA")
        conflict.write_bytes(b"BBBB")
        state = context(root, base, conflict)
        namespace["scan_conflicts"](state)
        assert state.conflict_map == {str(conflict): [str(conflict)]}
        results.append({"scenario": "extensionless_original_scan",
                        "result": "conflict grouped as its own original; README not grouped"})

        if hasattr(os, "symlink"):
            root, base, conflict = fixture(parent, "symlink-original")
            outside = parent / "outside-selected-folder.txt"
            outside.write_bytes(b"SAFE")
            base.unlink()
            base.symlink_to(outside)
            state = context(root, base, conflict)
            namespace["scan_conflicts"](state)
            assert str(base) in state.conflict_map
            namespace["accept_version"](state, str(conflict), str(base))
            assert outside.read_bytes() == b"BBBB"
            results.append({"scenario": "original_is_symlink_outside_selected_folder",
                            "result": "scan includes original; acceptance writes to symlink target"})

    print(json.dumps({"upstream_commit": COMMIT, "source_sha256": SOURCE_SHA256,
                      "host": platform.platform(), "python": platform.python_version(),
                      "method": "unchanged AST action/scan bodies; GUI-only callbacks stubbed",
                      "synthetic_temporary_directories_removed": True, "results": results}, indent=2))


if __name__ == "__main__":
    main()
