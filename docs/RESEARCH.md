# Why build ConflictBench?

Research date: **2026-10-08**. This is a bounded comparison of public primary sources and one reproducible synthetic experiment, not a market-size study or an exhaustive audit of all Syncthing software.

**Verdict: a small, defensible gap exists.** Conflict listing, manual version selection, and text diffs already exist. The reason to install another application is narrower: review arbitrary local documents on multiple desktop operating systems, make an explicit choice, reject changes since review, preserve the pre-operation bytes outside the sync tree, and record a recoverable transaction. This conclusion supports building and testing that workflow; it does not establish demand for every proposed feature or prove the new application is safe before its own tests pass.

## Evidence of a user problem

- A January 2025 [Syncthing forum request](https://forum.syncthing.net/t/better-conflict-resolution-in-ui/23689) describes three computers, an intermittently offline laptop, accumulated conflicts, and uncertainty about what to keep. Replies point to SyncTrayzor and explain why automatic newest/oldest/largest rules cannot know the user's intended contents. This is direct qualitative demand, not evidence of a large market.
- [Syncthing issue #8114](https://github.com/syncthing/syncthing/issues/8114), opened in January 2022, requests conflict counts in the web UI. It was shown as open when checked. [Syncthing Tray issue #140](https://github.com/Martchus/syncthingtray/issues/140), opened in June 2022, similarly requests conflict visibility and was shown as open. An open issue is evidence of an unresolved request, not proof that every related feature is absent.
- Syncthing's [synchronization documentation](https://docs.syncthing.net/users/syncing.html#conflicting-changes) explains the `.sync-conflict-<date>-<time>-<modifiedBy>` naming convention and that conflict copies subsequently synchronize as ordinary files. A local deletion or replacement can therefore propagate to other devices. Its case-conflict section describes a separate filename-compatibility problem; a resolver must not silently treat `file.txt` and `FILE.txt` as interchangeable versions.
- Syncthing's [versioning documentation](https://docs.syncthing.net/users/versioning.html) says versioning is per device and folder, is disabled by default, and archives incoming changes from other devices. It does not archive that device's own local edits. Consequently, enabling Syncthing versioning is not a replacement for backing up bytes before a local resolver changes them.

## What existing tools already do well

| Alternative, pinned revision | Existing strengths | Relevant boundary observed |
| --- | --- | --- |
| [Python Syncthing Deconflicter](https://github.com/cmprmsd/syncthing-deconflicter/tree/47438c05f7ad949b8062cdba2324c53d23efb638) | Small local-folder PyQt6/QScintilla GUI; side-by-side text highlighting; editable base and copying selected text; additional conflicts shown. No Syncthing API configuration in its code. | Text editor round-trip and direct writes/deletes. Synthetic action-level findings below. No packaged cross-OS execution was tested. |
| [SyncTrayzorV2 fork named in the brief](https://github.com/dlgcy/SyncTrayzorV2/tree/442d88bb157c8f2edc65f96b525b251a22b1e54b) | Integrated Windows Syncthing wrapper, conflict alerts, choosing versions, device and file metadata, optional recycling of removed files. | Windows application. Resolver method does sequential deletion/rename and has no reviewed-byte hash or transaction journal in the inspected path. |
| [Current SyncTrayzor upstream](https://github.com/GermanCoding/SyncTrayzor/tree/1344bf89cb8ec1e3f9918e85f71978edf30beb2f) | Maintained continuation; same Windows integration and recycling option. Its newer parser supports extensionless conflicts, unlike the older fork's expression. | The inspected resolution sequence is still delete(s), then move chosen file. A caught failure displays an error. This was source inspection, not a Windows execution test. |
| [Obsidian Syncthing Manager](https://github.com/gustjose/obsidian-syncthing-manager/tree/0e1e612982b21a27323e2f62a228689cdcdc6a13) | Strong fit for Obsidian vaults: CodeMirror comparison/merge, sync status, API-connected controls, mobile support and version-history integration. Removal uses Obsidian's trash API. | Designed around Obsidian's vault and API setup. The inspected accept path trashes the original then renames the conflict; save-merge writes editor text. No reviewed-byte hash or operation journal appears in those paths. |
| [syncthing-resolve-conflicts Bash tool](https://github.com/dschrempf/syncthing-resolve-conflicts/tree/ee1c5536924e64f9ba8d9a0edd834e37180ed1fa) | Established command-line option with selectable search scope, external merge program, and interactive choices. Documents handling of recursive conflicts and non-text files. | README routes non-text files to an explicit removal prompt. The inspected script uses direct `rm`/`mv` operations; it does not supply the proposed desktop preview and journal workflow. Not executed in this study. |

Do not advertise ConflictBench as the first conflict resolver, first diff UI, or first tool with any recovery capability. SyncTrayzor's recycling option and Obsidian's trash/versioning integration are real alternatives. For a Windows user already satisfied with SyncTrayzor, or an Obsidian-only user happy with that plugin, another installation may offer insufficient benefit.

The bounded target is a person with accumulated conflicts across arbitrary local documents who wants an occasional, account-free review tool with consistent behavior across desktop systems. Built-in merging, background monitoring, automatic newest-wins rules, and replacing Syncthing are not necessary to establish that value.

## Actual synthetic reproduction

The experiment ran on macOS arm64 with Python 3.14.6. Every file was generated under a `TemporaryDirectory`; no real user or synced directory was read or mutated. All test directories were automatically removed. The downloaded competitor was **not launched as a GUI**, installed, or modified.

The [harness](reproduce_deconflicter.py) verifies the complete upstream source's SHA-256, extracts the unchanged `scan_conflicts`, `accept_version`, and `accept_and_cleanup` method bodies through Python's AST, then executes those bodies. Only GUI message boxes, list widgets, and post-action screen refresh callbacks are stubbed. The binary scenario reproduces the exact UTF-8 replacement/split/join pipeline used by its preview; it does not exercise QScintilla rendering. These are action-level results, not a complete end-to-end usability test.

Pinned upstream source: [`deconflicter.py` at 47438c05f7ad949b8062cdba2324c53d23efb638](https://github.com/cmprmsd/syncthing-deconflicter/blob/47438c05f7ad949b8062cdba2324c53d23efb638/deconflicter.py). SHA-256: `e89de4fcb7fb207c53d9193dfcc8a03d81499523c388e9f9b8a924fa6035dc54`.

| Synthetic case | Observed result in the pinned Python actions | Required ConflictBench behavior to fill the gap |
| --- | --- | --- |
| Conflict source changes `BBBB` → `CCCC` after review; size and modification time restored | Acceptance writes unreviewed `CCCC`; no rejection. | Compare full content hashes against the reviewed snapshot and abort. |
| Original changes `AAAA` → `DDDD` after review; size and modification time restored | Selecting the conflict overwrites the later `DDDD`. | Revalidate every affected version, including the destination. |
| Original changes after text preview, then user accepts base | Cached `AAAA` overwrites `DDDD`; conflict copy is deleted; no backup created. | Keeping an original preserves its actual reviewed bytes, without re-encoding display text. |
| Selected conflict disappears before acceptance | `FileNotFoundError` occurs after the original has been truncated to zero bytes. | Missing source aborts before changing the original; prepared backup/journal records permit recovery from later failures. |
| Binary original `ff 00 fe 01`; accept-base with no text edits | Output is `ef bf bd 00 ef bf bd 01`; conflict is deleted. | Treat unknown/binary content as bytes, offer explicit choices, and never save the preview as a conversion. |
| Extensionless `README` plus matching conflict | Scanner groups the conflict as its own original instead of grouping `README`. | Parse the supported naming grammar, including extensionless files, and reject ambiguous matches. |
| Original is a symlink to a sibling outside the selected folder | Scanner includes it; acceptance overwrites the symlink target. | Reject symlink/reparse-point paths and escapes at scan and operation boundaries. |

Machine-readable observations, source hash, and runtime are in [deconflicter-evidence.json](deconflicter-evidence.json). These findings are not allegations that the existing project promises this safety model. They explain why a different workflow can be useful.

To reproduce from the repository root with Python 3 on a host that permits symlinks:

```sh
curl -fsSL https://raw.githubusercontent.com/cmprmsd/syncthing-deconflicter/47438c05f7ad949b8062cdba2324c53d23efb638/deconflicter.py -o /tmp/deconflicter-pinned.py
python3 docs/reproduce_deconflicter.py /tmp/deconflicter-pinned.py
```

The harness deliberately refuses different source bytes. It has no PyQt dependency and does not download or run any GUI package. Remove the downloaded `/tmp/deconflicter-pinned.py` after use. Upstream code is linked and fetched by the reviewer; no upstream implementation is bundled in this repository.

## Claims and release gates

The right promise is **a reviewed local operation with explicit failure and recovery states**. SHA-256 revalidation alone cannot make an operation atomic against an uncooperative sync engine or editor. Pause the affected Syncthing folder, stop editing these files, and require the user to acknowledge that condition before committing. A local tool cannot prove that remote devices have stopped changing files. It must not claim to undo later changes on another device.

The following are product acceptance criteria, not results of the competitor experiment:

1. Read-only, user-selected-folder scan; no whole-home scan. Exclude Syncthing internal/version storage. Group safe supported names with dot, Unicode, case, extensionless, missing-original and malformed-name fixtures; disclose unsupported cases.
2. Review exact immutable version snapshots or bind every displayed version and decision to its hash. Support explicit original/conflict/keep-both choices for binary files. Never infer correctness from modification time or file size.
3. Preview the paths and consequences before writes. Require a backup destination outside the selected sync tree, reject symlink escapes, and preserve confidentiality of backup bytes and names. A separate backup volume may fail or fill up; successful backup verification must precede destructive steps.
4. Revalidate at commit, prevent concurrent workbench instances for the same scope, and test same-size changes. A process lock cannot lock out Syncthing, an editor, or a remote machine; document that limit.
5. Record durable stage/status before changing files. Verify replacement bytes. If several changes cannot be atomic together, leave a clearly incomplete/recoverable transaction after a failure or crash; never report success merely because one rename worked.
6. Test denied access, insufficient space, cancellation, interruption/restart, invalid backup destinations and external-tool failures with disposable fixtures. External diff executables must receive explicit argument arrays, without shell evaluation; a diff tool's own writes must invalidate the reviewed plan.
7. Undo only when the current paths still match the transaction's expected post-state. Refuse later edits and describe the action as a local restoration. Keep readable receipts and backup format documentation so recovery does not depend solely on the GUI.
8. Verify actual macOS, Windows and Linux filesystem behavior and packaged first-run flows. Source portability, a Qt dependency, or the synthetic Python result is not proof of those gates passing.

The strongest falsification test is a same-size, same-mtime post-review edit followed by interrupted replacement and restart: if ConflictBench overwrites unreviewed contents, loses pre-operation bytes, or reports a partial transaction as successful, it has not filled the identified gap. No extra feature compensates for that failure.

## Source inspection references

- Python: [`load_views` and action methods](https://github.com/cmprmsd/syncthing-deconflicter/blob/47438c05f7ad949b8062cdba2324c53d23efb638/deconflicter.py#L132-L203).
- Supplied SyncTrayzor fork: [`ConflictFileManager.ResolveConflict`](https://github.com/dlgcy/SyncTrayzorV2/blob/442d88bb157c8f2edc65f96b525b251a22b1e54b/src/SyncTrayzor/Services/Conflicts/ConflictFileManager.cs#L353-L391).
- Current SyncTrayzor upstream: [`ConflictFileManager`](https://github.com/GermanCoding/SyncTrayzor/blob/1344bf89cb8ec1e3f9918e85f71978edf30beb2f/src/SyncTrayzor/Services/Conflicts/ConflictFileManager.cs#L353-L391) and [error handling / recycling option](https://github.com/GermanCoding/SyncTrayzor/blob/1344bf89cb8ec1e3f9918e85f71978edf30beb2f/src/SyncTrayzor/Pages/ConflictResolution/ConflictResolutionViewModel.cs).
- Obsidian: [`conflict-manager.ts`](https://github.com/gustjose/obsidian-syncthing-manager/blob/0e1e612982b21a27323e2f62a228689cdcdc6a13/src/services/conflict-manager.ts) and [`conflict-modal.ts`](https://github.com/gustjose/obsidian-syncthing-manager/blob/0e1e612982b21a27323e2f62a228689cdcdc6a13/src/ui/conflict-modal.ts#L182-L211).
- Bash tool: [README](https://github.com/dschrempf/syncthing-resolve-conflicts/blob/ee1c5536924e64f9ba8d9a0edd834e37180ed1fa/README.md) and [script](https://github.com/dschrempf/syncthing-resolve-conflicts/blob/ee1c5536924e64f9ba8d9a0edd834e37180ed1fa/syncthing-resolve-conflicts).

Only the Python method experiment was executed. The other comparisons are limited source/documentation inspections. No live Syncthing cluster, competing Windows application, Obsidian installation, or multi-device undo was tested in this research. Source availability and revisions can change; these conclusions are tied to the links above.
