# Android 16 memory.reclaim hardening backport

## A16/A17 structural differences

- Android 16 has the same `CompactMemcgAction` type, constructor/member layout, `FULL`/`ANON`/`FILE` modes, UID/process path construction, and `WriteStringToFile`-based errno handling as the Android 17 pre-patch implementation.
- `SetAttributeAction` is structurally separate from reclaim handling and required no change.
- Android 16 uses the same `access()` path-existence check and the same `memory.current`/`memory.reclaim` path construction needed by the Android 17 behavior.
- The test target and existing fixtures/helpers are compatible with the Android 17 tests. Android 16 needed the same `android-base` file/test utility and `<filesystem>` includes, but no new build dependency or API.
- The only incidental source difference relevant to patch placement is that the A17 patch context already contained `<cinttypes>`; the A16 file does not. `<cstring>` was added in the appropriate standard-library include block.

## Files changed

- `libprocessgroup/task_profiles.cpp`
- `libprocessgroup/task_profiles.h`
- `libprocessgroup/task_profiles_test.cpp`
- `.codex-reference/a16-memory-reclaim-backport.md` (this untracked report)

## Semantic mapping

- Added a per-`CompactMemcgAction` atomic `UNKNOWN`/`SUPPORTED`/`UNSUPPORTED` cache for the optional swappiness grammar.
- ANON/FILE success and `EAGAIN` record support. `EINVAL` and `EOPNOTSUPP` record unsupported and return failure. Cached unsupported actions fail without another write and never fall back to full reclaim.
- Full reclaim does not use the optional-syntax cache. Its `EINVAL` remains a logged failure; its `EOPNOTSUPP` remains best-effort success as in A17.
- A missing `memory.reclaim` is best-effort success only when `memory.current` still proves the memcg exists. Missing `memory.current` and other write/path failures remain failures.
- `EAGAIN` remains one-shot best-effort success; no retry, loop, sleep, or backoff was added.
- Relaxed atomic operations and compare/exchange match A17: state is action-local, thread-safe, and no lock spans filesystem operations.

## Behavior already present in A16

- Reclaim payload generation already used the complete `memory.current` value, with `swappiness=200` for ANON and `swappiness=0` for FILE.
- `EAGAIN` was already treated as best-effort success without retry.
- UID/process cgroup path construction and the initial optional-syntax probe already existed.

## Tests added

Added the A17 host-independent focused coverage for:

- missing `memory.reclaim` validation and best-effort execution with a surviving memcg;
- full reclaim payload;
- successful ANON and FILE optional payloads;
- caching of supported optional syntax (no repeated validation write);
- missing `memory.current` as an error;
- genuine reclaim write failure as an error.

The existing regular-file fixture cannot synthesize kernel `EINVAL`, `EOPNOTSUPP`, or `EAGAIN` responses. No write-function injection seam was added because neither A16 nor the authoritative A17 patch has one, and fabricating it would expand the backport beyond the requested minimal structure. Those paths are implemented directly from A17 and were manually reviewed.

## Validation

- `git diff --check`: passed.
- Attempted `source build/envsetup.sh && OUT_DIR=/tmp/losp-a16-core-out m task_profiles_test` from `/home/BUILDS/repos/LOSP-CR`: could not run because the available checkout is not recognized as an Android source-tree root (`Couldn't locate the top of the tree`). Therefore no test binary was built or executed.
- Reviewed the final diff for errno separation, no ANON/FILE-to-FULL fallback, per-action cache lifetime, relaxed-atomic thread safety, false-success paths, and changes outside memory reclaim.

## Limitations

- Runtime tests were unavailable in this standalone/partial checkout.
- Kernel-specific errno branches are not directly exercised by the host-independent regular-file tests, matching the authoritative A17 patch's test scope.

## Intentionally not ported

- No A3 task-profile execution/cache changes.
- No device policy, JSON configuration, unrelated task-profile behavior, cleanup, or refactoring.
- No test-only syscall/write interception infrastructure.

## Recommended commit subject

`libprocessgroup: harden optional memory.reclaim handling`

## Recommended commit body

```text
Treat a missing memory.reclaim file as an unavailable optional capability
when the target memcg still exists, while preserving errors for missing
cgroups and genuine write failures.

Classify EINVAL only for the optional swappiness syntax, handle
EOPNOTSUPP explicitly, and cache syntax support per CompactMemcgAction to
avoid repeated failed probes and duplicate logs. Keep EAGAIN as the
existing best-effort partial-reclaim result without adding teardown delay.

Add host-independent coverage for missing and present reclaim files,
full/anon/file payload generation, cached syntax support, and genuine path
failures.
```

## git status --short

```text
 M libprocessgroup/task_profiles.cpp
 M libprocessgroup/task_profiles.h
 M libprocessgroup/task_profiles_test.cpp
?? .codex-reference/
```
