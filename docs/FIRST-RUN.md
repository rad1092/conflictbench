# Try a decision and undo it

ConflictBench is for a Syncthing user who needs to inspect competing local
versions, rather than choose by timestamp. It does not connect to Syncthing's
API, pause it for you, or decide which content is correct.

1. Download and extract the archive for your platform from
   [Releases](https://github.com/rad1092/conflictbench/releases). Check its SHA-256
   against `SHA256SUMS`. Open ConflictBench and choose **Try demo**.
2. Select a text conflict. Compare **Versions** and **Text difference**, then
   choose **Keep both**. Read the proposed new filename in **Review plan**.
3. This demo uses disposable, app-created files and an external demo backup.
   Review the two acknowledgments and commit. The original stays, a separate
   copy holds the conflict bytes, and the conflict-marker file disappears.
4. Open **History / recovery**, select the receipt, and use
   **Undo / recover selected**. With these unchanged demo files, the original
   pair should return. A failed recovery is a reason to inspect the receipt,
   not to force another overwrite.

Demo files and their backups disappear when the app closes. Do not put real
documents in the demo folder. On a real folder, pause synchronization in
Syncthing, close editors, and select an existing private backup folder outside
**all** synced folders before acknowledging the plan. Review the resulting
files before resuming sync.

Syncthing's own file versioning can be another recovery layer, but it is not
ConflictBench's transaction backup and does not remove the pause requirement.
Neither tool can undo edits that have already propagated to another device by
simply restoring local bytes. See [Safety](SAFETY.md).

## If a decision is blocked

| Symptom | What to check |
| --- | --- |
| No conflicts listed | Select the actual local folder containing Syncthing conflict-marker files. Missing originals and case-only collisions are outside scope. |
| Preview is partial | Read its size/line-limit notice. Matching visible prefixes do not prove identical files. |
| Files changed / rescan required | Leave the files alone, pause writers and scan again. Approve the newly reviewed hashes. |
| Backup rejected | Use a private local directory outside every sync root, including when scanning a subfolder. |
| Transaction lock unavailable | Close another active ConflictBench transaction and verify local temporary-directory permissions. Do not delete a live lock to force progress. |
| Recovery refuses changed files | Preserve the receipt and backups; inspect the later changes. Recovery deliberately refuses to overwrite an unrecognized state. |

## Reproduced public-package check

On 2026-10-12, the SHA-256-verified public macOS arm64 0.1.0 ZIP was extracted
and launched on macOS 27.0.1. `--smoke-test` passed native scan, preview, commit
and undo using synthetic files. A fresh source build passed all three CTest
suites (core safety, desktop smoke and desktop workflow), including all three
decisions, stale-content refusal and recovery boundaries. No live synced folder
was changed. This does not claim testing against a running Syncthing service or
human testing of every platform's file picker.
