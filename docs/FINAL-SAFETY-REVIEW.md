# Final bounded core safety review

Reviewed 2026-10-08 on macOS 27.0.1 arm64, Apple Clang 21.0.0 and shared Qt 6.11.2. This supplements [CORE-REVIEW](CORE-REVIEW.md). Scope: transaction stages, conditional recovery, receipt paths, cancellation, and Windows metadata handling in `core.cpp`, with maintained tests and disposable synthetic probes. It does not certify GUI/package behavior or substitute for the final commit's three-OS CI.

**Result:** the reproduced recovery blocker below is fixed in the tested snapshot. The independent macOS probe confirms refusal without overwriting or removing the later files. The maintained core suite built from the same copied sources reports **64 passed, 0 failed, 2 Windows-only skips**. Windows EA and native ACL/junction behavior still require successful Windows CI; no Windows runtime result is claimed here.

| Independently built file | SHA-256 |
| --- | --- |
| `src/core.cpp` | `9e05e04c6eacd7e0783b0f983e675214e7fdda7f59f8a55c4c6c2cfa221a6b81` |
| `src/core.h` | `7db4a354bcd433a38e80e65664451367cd480681e8e9c5b8bb80f2cd94341864` |
| `tests/core_tests.cpp` | `9c27b91ba841d4cf1d051e66e91d6c0e27a029d73af91adb9b5fbfa12918f353` |

Later source changes are not implicitly covered. The review began before the initial Git commit, so hashes identify the actual snapshot. Builds used one compiler process and copied sources in a dedicated temporary directory. No personal documents were scanned or changed.

## Reproduced and resolved recovery blocker

In the initial snapshot (`core.cpp` SHA-256 `e48f9b7ef97c28b0ec788f298923769ec5cd007991de8fc6d156f6a64fe3ce64`), `writing_destination` recovery accepted either known original contents, or a keep-both destination whose bytes matched the conflict, without a recorded post-write snapshot. This was too permissive even without receipt tampering.

Reproduction used one original containing `old version\n` and one conflict containing `new version\n`. A fault on the third `before_copy_commit` interrupted execution after both backups, but before destination rename. The durable state was `writing_destination`; the original was still unchanged. The probe then simulated a later user's action by writing the conflict bytes to the original, or creating the planned keep-both destination, and advancing its modification time by seven seconds.

| Synthetic later action | Initial recovery result | Retest after fix |
| --- | --- | --- |
| Replace original with the conflict bytes | Returned success and replaced the later original with old bytes | Refused recovery; preserved `new version\n` |
| Create the planned keep-both file with conflict bytes | Returned success and deleted the later file | Refused recovery; file remained with `new version\n` |

The implementation owner changed this stage to require both exact pre-operation snapshots and an absent keep-both destination. A crash after rename but before durable post-state now requires manual recovery from the verified backups. This is intentional: matching content cannot identify who wrote the file. Maintained regressions are `unrecordedDestinationRefusesLaterMatchingWrite` for both actions.

Other destructive undo windows were inspected. A possibly restored conflict is accepted without rewriting it. An original already containing the old bytes is likewise left intact. Removal of a keep-both copy during resumed undo still requires its recorded undo-start snapshot. No additional destructive-stage blocker was reproduced.

## Windows extended attributes

Code inspection found that the Windows branch refused alternate data streams but did not inspect NTFS extended attributes. These are different metadata facilities: [FindFirstStreamW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirststreamw) enumerates `$DATA` streams, while [FILE_EA_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_ea_information) reports application-specific extended attributes. A practical example is [WSL permission metadata](https://learn.microsoft.com/en-us/windows/wsl/file-permissions), including `$LXUID`, `$LXGID` and `$LXMOD`. A byte-only replacement does not preserve those values.

With the implementation owner's coordination, this reviewer added a fail-closed query on the already opened identity handle. It requests `FILE_READ_EA` and uses the documented [NtQueryInformationFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntqueryinformationfile) with [FileEaInformation, value 7](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_file_information_class). The function is resolved only from already loaded system `ntdll.dll`. There is no DLL search, privilege enabling or fallback that ignores a failed query. A missing API, failed/short result, or nonzero EA size refuses the file before mutation. Every subsequent snapshot repeats that inspection.

`windowsExtendedAttributesRefused` adds a synthetic EA using the native equivalent of [ZwSetEaFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwseteafile), then checks original and conflict separately, both before scanning and after reviewing a plan. It asserts refusal and unchanged main bytes, with no transaction created. This test is Windows-only. **The reviewer implemented this patch; it is not independently runtime-validated by the macOS result.** Successful Windows compilation and execution are release gates.

Windows owner/group/DACL preservation, alternate-stream rejection, private backup DACLs and junction checks already have maintained tests. Audit SACLs, mandatory integrity labels and other security metadata beyond owner/group/DACL are not preserved or comprehensively detected. Files relying on such policies must remain outside the supported scope; do not market this as a complete Windows security-descriptor archive.

## Other executed checks and limits

The independent four-case probe also confirmed that cancellation after a recorded destination leaves a recoverable interrupted transaction, and that interruption after undo's original restoration can be retried. Both original and conflict bytes were restored in those cases.

The full maintained suite in the copied snapshot additionally passed normalized receipt traversal refusal, source and preview revalidation, same-size mutation with restored timestamp, symlink/ancestor/hard-link refusal, backup placement and permissions, malformed receipts, external changes, corrupt backups, crash/restart and transaction serialization. The two skips were Windows EA and native permissions/junction tests.

Receipts remain trusted private local state, not authenticated instructions. Normalization prevents the demonstrated `..` escape; it does not authenticate a receipt deliberately rewritten to identify another root and matching files. Users must not import somebody else's receipt or edit one to bypass a refusal.

Pause Syncthing and all other writers throughout commit and recovery. Hashes, permission checks and the per-user lock cannot exclude a noncooperating process in the final validation-to-rename/remove interval. Multi-file operations are not atomic, and file/directory flushes do not guarantee survival of every power-loss or filesystem failure. Windows has no claimed portable directory-flush guarantee. Recovery is local, conditional, and does not reverse propagation to other devices. Full metadata preservation and original modification-time restoration are not claimed.

Temporary copied source, custom probe, build products and disposable fixtures were removed after these results were recorded. The shared Qt test-mode lock directory was left alone because other test processes may use it.

## CI follow-up: private lock bootstrap

The release owner subsequently reported that actual macOS 15 CI could not create the transaction lock beneath its ACL-bearing `Library/Application Support` parent. Core tests used Qt's test-mode directory, so that host-specific startup condition had not been covered by their earlier success. This reviewer implemented a separate internal lock bootstrap; backup directory creation retains its original conservative parent-ACL refusal.

The bootstrap creates a new empty lock directory with mode 0700 on POSIX, opens that directory without following a leaf symlink, and removes only its own inherited ACL through the descriptor. macOS uses an empty extended ACL; Linux removes inherited access/default ACL xattrs. It reapplies mode 0700 and verifies directory ownership, mode and absence of extended ACLs before creating a lock. Existing directories are validated and refused if unsuitable; they are never repaired automatically. No parent, source or backup directory ACL is changed. The production location remains one fixed global per-user directory regardless of selected source or backup roots.

Windows continues to create a protected DACL granting the current user full access. The creation descriptor now explicitly specifies that user's SID as owner, avoiding an elevated token's potentially different default owner. Existing lock directories are checked for the current owner and a protected, single-user DACL. `windowsLockBootstrapKeepsProtectedPerUserAcl` verifies creation beneath a broader parent, safe reuse, and refusal without mutation after an existing directory's DACL is broadened. Windows runtime execution remains a CI requirement.

`lockBootstrapClearsOnlyNewDirectoryAcl` passed on macOS with a synthetic parent containing an inheritable everyone-read/search ACL: the new child had mode 0700 with no extended ACL, the parent's ACL was byte-for-byte unchanged in its text representation, safe reuse succeeded, and an existing broader directory was refused without alteration. The pre-existing backup ACL refusal test also passed. The Linux variant creates a synthetic default POSIX ACL and checks that the child loses both access/default ACL xattrs while the parent retains its default ACL; its execution belongs to Linux CI.

Syncthing control filenames and temporary prefixes are now excluded case-insensitively on all platforms, including `.STIGNORE`, `.SYNCTHING.*` and `~SYNCTHING~*`, to avoid aliases of engine control files on case-insensitive volumes. Exact-case pairing of ordinary documents is unchanged.

After these changes, a separate single-process macOS build of the maintained core suite reported **68 passed, 0 failed, 3 Windows-only skips**. The three skips were protected lock DACL, extended attributes, and native permissions/junction tests. The tested hashes are `d9480a90a79e1046eac34dac885abe545853e4aada585108c14bb54fac5819fc` for `src/core.cpp` and `523afecc1b585f58458502f608a3ec3fefd70dc96dfe0b00fa3996f899c23749` for `tests/core_tests.cpp`. The temporary follow-up build was removed. A passing macOS 15 application smoke and the final multi-OS release commit's CI must be recorded separately.
