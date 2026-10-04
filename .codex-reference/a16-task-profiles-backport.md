# Android 16 libprocessgroup A3 backport report

## A16/A17 structural differences

The Android 16 implementation has the same relevant architecture as the Android 17 reference: `TaskProfile` objects are shared through `std::shared_ptr`, aggregate profiles retain references to those objects, actions own separate task/process FD slots guarded by per-action mutexes, and profile overlays replace actions in place through `TaskProfile::MoveTo` so existing aggregate references remain valid.

The A16 defects matched the A17 pre-patch implementation: one unsynchronized `TaskProfile::res_cached_` flag represented both task and process caches; `MoveTo` copied that stale flag; aggregate, UID, and process-thread execution could report false success; cpuset ENOSPC suppression used a racy static bool and converted later failures to success; and scheduler failure logging dereferenced an absent optional Nice value.

A16 already contains the separately ported A1 `CompactMemcgAction` implementation and tests. That code differs from the A17 A3 patch context but is not part of A3 and was left unchanged.

## Files changed

- `libprocessgroup/cgroup_map.cpp`
- `libprocessgroup/task_profiles.cpp`
- `libprocessgroup/task_profiles.h`
- `libprocessgroup/task_profiles_test.cpp`
- `.codex-reference/a16-task-profiles-backport.md`

## Semantic mapping from Android 17 A3

- Replaced literal UID/PID regex substitution with `android::base::StringReplace(..., true)`.
- Split profile-level cache state by `ResourceCacheType` and serialized enable/drop transitions.
- Reset cache state when overlay actions replace an existing profile.
- Propagated aggregate, user-profile, and process-wide per-thread failures without short-circuiting the intended iteration.
- Made optional Nice failure handling safe.
- Made cpuset ENOSPC report-once state atomic while preserving failure returns.
- Ported the focused A17 tests and added an overlay cache reset test; the aggregate test also checks duplicate execution.

## Behavior already present in A16

- `SetCgroupAction` and `WriteFileAction` already had separate task/process FD arrays and per-action FD mutexes.
- `FdCacheHelper::IsAppDependentPath` already prevented caching paths containing `<uid>` or `<pid>`.
- `WriteFileAction` value substitution already used literal, replace-all `StringReplace` calls.
- Process/profile iteration order was already correct; only accumulated result propagation was missing.
- `SetProcessProfiles` and `SetTaskProfiles` already accumulated failures and used `LOG` for logical failures.

## Path substitution findings

`CgroupControllerWrapper::GetProcsFilePath` constructed two `std::regex` objects per call for literal `<uid>` and `<pid>` replacement. It now uses `StringReplace` with `all = true`, preserving literal replace-all output and path semantics without regex interpretation. `WriteFileAction` already used the desired helper and was unchanged.

## Task/process cache separation

The single `bool res_cached_` was replaced by one flag per cache type. Enabling or dropping the task cache no longer marks or clears the process cache, and vice versa. Invalid cache types are rejected before array access.

## Cache lifecycle and overlay behavior

Cache enable/drop transitions are protected by a per-profile mutex. Ordinary execution continues to use only the existing per-action FD mutexes. `MoveTo` replaces the old actions and resets both cache flags instead of inheriting stale state. Destruction of replaced actions closes their cached FDs. Dynamic application-dependent paths retain the existing uncached sentinel behavior; caching was not broadened.

## Aggregate action behavior

`ApplyProfileAction` still executes child profiles in vector order and does not short-circuit. It now accumulates results and returns false if any child fails. Duplicate profile entries remain duplicate executions.

## SetUserProfiles findings

`SetUserProfiles` now accumulates missing-profile and execution failures and returns false if any occur while retaining iteration order. Logical failures use `LOG`, not `PLOG`, and the messages correctly say `user profile` with proper spacing.

## WriteFileAction findings

When no process path is configured, process-mode execution still visits every numeric `/proc/<pid>/task` entry. It now accumulates per-thread write results and returns false after any partial failure without stopping later attempts.

## Scheduler/Nice findings

The Nice/priority representation is the same `std::optional<int>` used by A17. `ExecuteForTask` now uses `value_or(0)` for scheduler setup and failure logging, so a normal policy with no Nice value cannot dereference an empty optional. Scheduler and Nice policy behavior is otherwise unchanged.

## cpuset ENOSPC findings

The report-once flag is now `std::atomic_bool` with a relaxed atomic exchange. Only duplicate log emission is suppressed. Every ENOSPC occurrence still reaches the final false return; later occurrences are no longer converted to success.

## Concurrency review

Shared `TaskProfile` cache transitions are serialized by a per-instance mutex. The mutex is held while invoking action cache-management methods, which take their existing per-action FD mutexes; ordinary profile execution does not take the profile cache mutex. There is no inverse action-FD-to-profile-cache lock path in this implementation. The cpuset report-once state has no data race. Overlay replacement occurs during profile loading and resets stale cache metadata; no global or execution-wide mutex was added.

## Tests added/updated

- Concurrent task/process cache enable and independent drop behavior.
- Cache-state reset after action replacement.
- Aggregate failure propagation, continued later execution, and duplicate preservation.
- Process-mode `WriteFileAction` failure propagation.
- Normal scheduler policy failure with absent Nice.
- Existing A1 reclaim tests were retained unchanged.

Direct unit tests for private loader-driven `SetUserProfiles`, controller-backed path construction, and the kernel-specific cpuset ENOSPC branch would require test seams or mocking infrastructure absent from this A16 test target; the production changes follow the authoritative A17 implementation exactly.

## Validation performed

- `git diff --check`: passed.
- Reviewed the complete scoped diff against the Android 17 reference patch.
- Reviewed cache lock ordering, all changed false-success paths, stale errno logging, optional access, report-once atomicity, and the unchanged A1 reclaim block.
- Android `task_profiles_test`: not run because this repository is a standalone `system/core` checkout without the Android build/Soong launcher, `m`, `atest`, or a prebuilt test binary.

## Limitations

No executable Android unit-test result is available from this standalone checkout. The focused tests were source-ported but require a complete Android build environment to compile and run. No new mocking framework was introduced for kernel/controller-specific branches.

## Intentionally not ported

No JSON policy, hierarchy policy, device/vendor configuration, scheduler choice, cpuset placement rule, A2 boot-prefetch code, or unrelated refactor was changed. No A17 structure beyond the A3 patch semantics was introduced.

## A1 reclaim confirmation

A1 reclaim semantics remain intact and unchanged: the UNKNOWN/SUPPORTED/UNSUPPORTED optional syntax cache, selective EINVAL/EOPNOTSUPP handling, EAGAIN best-effort behavior, missing `memory.reclaim` behavior, and prohibition on ANON/FILE-to-FULL fallback are preserved exactly.

## Recommended commit subject

`libprocessgroup: backport task profile execution hardening`

## Recommended commit body

```text
Keep task and process resource-cache state independent and serialize cache
enable/drop transitions for shared TaskProfile instances. Reset cache state
when overlays replace profile actions while retaining action-level FD locking
and uncached dynamic application paths.

Propagate failures from aggregate, user-profile, and process-wide WriteFile
execution without changing ordering or suppressing later attempts. Make
optional Nice logging safe and make cpuset ENOSPC report-once handling atomic
without turning repeated failures into success.

Replace per-call UID/PID regex construction with literal replacement and add
focused cache, overlay, aggregate, WriteFile, and scheduler regression tests.
Preserve the existing A1 memory.reclaim behavior unchanged.
```

## git status --short

```text
 M libprocessgroup/cgroup_map.cpp
 M libprocessgroup/task_profiles.cpp
 M libprocessgroup/task_profiles.h
 M libprocessgroup/task_profiles_test.cpp
?? .codex-reference/
```
