# Real Linux disk-full validation

`tests/diskfull_test.cpp` exercises the actual `cbcore` transaction code against a dedicated 2 MiB tmpfs. `packaging/test_diskfull.sh` creates the mount under `RUNNER_TEMP`, runs the helper as the ordinary CI user, and unmounts it in an exit trap. Only the mount/unmount commands use `sudo -n`. The helper has a 30-second deadline and a 5-second termination grace period.

The helper refuses non-Linux builds, noncanonical paths, symlink aliases, a nonempty directory, a directory that is not a distinct mount root, a non-tmpfs filesystem, a mount not owned by the current user with mode 0700, or a volume outside 1–4 MiB. Both Linux `statfs` and Qt `QStorageInfo` must recognize the exact mount. It allocates at most the guarded volume size plus a small block, using real `write(2)` calls; sparse files and injected I/O errors are not used. A fill must fail with Linux `ENOSPC` (errno 28) or the test fails.

The fixtures and application lock are isolated in an owned `QTemporaryDir` beside the mount. The helper verifies that this directory is on another filesystem. The cleanup trap removes only the now-empty directories it created using `rmdir`; failed unmounts or unexpected leftover files are preserved and reported. The test never accepts a source folder or an existing backup directory from a user.

Two scenarios run:

1. **Backup volume full.** Original and conflict fixtures live outside tmpfs. After observing native `ENOSPC`, the helper truncates its own filler to reserve 160 KiB. The receipt and 64 KiB original backup must complete; a 512 KiB conflict backup must fail at the real `QSaveFile` data write. The second backup must not appear as a committed file. The durable receipt must remain `backing_up` and History must expose it.
2. **Destination volume full.** Both source fixtures live on tmpfs and the backups live outside it. The helper again fills to native `ENOSPC`, then reserves 64 KiB. Both backups must be intact, while the 512 KiB atomic replacement must fail at its real `QSaveFile` data write. The receipt must remain `writing_destination`.

In both cases `execute()` must report failure, never `committed`. Original and conflict bytes, inode identity, size, owner/group, mode, link count, and nanosecond modification time must remain unchanged. The helper removes only its own filler, explicitly calls `undo()` with the exact returned receipt, and verifies an `undone` receipt and unchanged source witnesses. JSON lines record mount size, native errno, free bytes, the application error, durable stages, and each recovery result. Any missing assertion or non-ENOSPC fill is a nonzero exit; CI must preserve its normal step log.

This is a real Linux capacity failure test, not evidence of Windows/macOS disk-full behavior or of whole-transaction atomicity. It does not simulate power loss, physical media faults, inode exhaustion, quotas, or hostile concurrent writers. Other safety and crash tests cover their explicitly named cases. The helper's native fill records errno; Qt's returned error text is recorded independently, since diagnostic strings may differ by Qt version. The original backup and durable-stage assertions ensure an unrelated early failure cannot pass.

## Build and CI integration

Inside the existing `if(BUILD_TESTING)` block, add a Linux-only helper target:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(diskfull_test tests/diskfull_test.cpp)
  target_link_libraries(diskfull_test PRIVATE cbcore)
endif()
```

Run the privileged mount fixture as a separate Linux CI step after the normal build, rather than silently requiring mount privileges in every `ctest` invocation:

```yaml
- name: Real disk-full transaction and recovery
  if: runner.os == 'Linux'
  shell: bash
  run: bash packaging/test_diskfull.sh "$PWD/build/diskfull_test"
```

On a dedicated local Linux test machine, set `RUNNER_TEMP` to an existing writable temporary directory and use the same command. Passwordless mount privileges are required; inability to mount is a failed validation, not a skipped pass. Do not run the helper against an existing user filesystem. Ordinary developers can continue running the regular core tests without `sudo`.
