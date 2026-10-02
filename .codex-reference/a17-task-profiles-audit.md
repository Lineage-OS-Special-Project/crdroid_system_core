# Android 17 generic libprocessgroup task-profile audit (A3)

## Scope and baseline

This audit covers the generic `system/core/libprocessgroup` task-profile mechanism, its configuration, focused tests, scheduler-policy bridge, cgroup map, and process-group lifecycle. It does not change device policy, profile assignments, Power HAL behavior, controller layout, CPU topology, scheduler tuning, or any JSON configuration.

A1's `memory.reclaim` capability handling was reviewed as part of the action audit and retained. A2 is independent of these paths.

## Architecture discovered

- `CgroupMap` is a deliberately leaked process-lifetime singleton. It loads immutable cgroup descriptors through `libprocessgroup_util`; lookups return lightweight wrappers over descriptors owned for the process lifetime.
- `TaskProfiles` is also a deliberately leaked process-lifetime singleton. Its constructor loads the system database, an optional first-API-level database, then vendor and system_ext overlays.
- Attributes resolve controller names during load and retain a controller wrapper plus v1/v2 filenames. Overlay attributes update the existing object in place so actions holding attribute pointers remain valid.
- Ordinary profiles own ordered `ProfileAction` objects. Ordinary profile overlays move replacement actions into the existing `TaskProfile`, preserving shared references held by aggregates.
- Aggregates resolve profile names to `shared_ptr<TaskProfile>` objects at load time and store those references in one `ApplyProfileAction`. Runtime application therefore does not redo name lookup or aggregate-name expansion. Nested aggregates remain a pre-resolved object graph.
- Public setters perform heterogeneous `std::map` lookup by `string_view`, optionally enable FD caching, and execute the selected profile for a task, process, or UID.
- `SetCgroupAction` and `WriteFileAction` cache only explicitly requested, non-application-dependent paths. Per-UID/PID paths are reopened. Attribute actions discover the target cgroup/path on each execution because membership and cgroup lifetime can change.
- The hot scheduler-policy compatibility entry points map policies to predeclared aggregate profile names and request task FD caching.
- Init creates UID/PID cgroups and activates v2 controllers before child-side process-profile application. Kill waits on `cgroup.events`, optionally reclaims (A1), and removes process cgroups; missing removal targets are idempotent.

## Hotspots found

- `CgroupControllerWrapper::GetProcsFilePath()` constructed two `std::regex` objects on every path expansion merely to replace the literal `<uid>` and `<pid>` tokens.
- A `TaskProfile` used one `res_cached_` boolean for both task and process caches. Enabling one type prevented the other from being initialized; dropping one type prevented the other from being dropped. The boolean was also read and written concurrently without synchronization even though profile objects are shared for the daemon/process lifetime.
- Aggregates are already name-resolved at load time; no repeated string expansion was found. Flattening them would complicate overlay reference semantics and was not justified.
- Stable non-app-dependent task/proc control files already have opt-in FD reuse. Dynamic attribute and app-cgroup paths are correctly reconstructed and reopened.

## Correctness issues found

- `ApplyProfileAction` always reported success even when a child profile failed.
- `SetUserProfiles` always returned success for unknown profiles and failed actions, and used `PLOG` despite not owning a meaningful errno.
- Process-mode `WriteFileAction` ignored every per-thread write result and always reported success after opening `/proc/<pid>/task`.
- `SetSchedulerPolicyAction` dereferenced an empty optional while formatting the failure log for normal policies without a `Nice` value.
- The cpuset `ENOSPC` report-once flag had a data race. Worse, every failure after the first was reported as success even though the task had not moved.
- Resource-cache bookkeeping had the independent-cache and data-race defects described above.

## Changes made

- Replaced runtime regex construction for UID/PID token expansion with literal `StringReplace` calls.
- Made task/process resource-cache state independent and guarded profile-level enable/drop transitions with a mutex. Action-level FD mutexes remain responsible for FD access.
- Reset replacement-profile cache state during load-time ordinary-profile overrides; new actions cannot inherit cache state belonging to destroyed actions.
- Propagated aggregate child failure while continuing to execute later children, preserving action order and best-effort aggregate traversal.
- Propagated UID-profile and process-wide `WriteFile` failures.
- Made scheduler failure logging safe when no nice value is configured.
- Made cpuset report-once state atomic and retained failure semantics for every `ENOSPC`.
- Added focused tests for independent concurrent cache initialization/drop, aggregate failure propagation without skipping later profiles, process-wide write failure propagation, and scheduler failure without an optional nice value.

## Changes deliberately rejected

- No aggregate action deduplication: repeated profiles/actions may be intentional and order is observable.
- No aggregate flattening: the existing shared-reference graph supports system/vendor override behavior and already avoids runtime name lookup.
- No global attribute-capability cache: attribute availability can vary by hierarchy, activation depth, and recreated cgroup. A global negative result would become stale.
- No cached FDs for UID/PID cgroups or dynamically discovered attribute paths: cgroups can disappear and be recreated, tasks move, and controller activation changes.
- No broader JSON strictness: unknown/missing vendor controllers and actions are currently compatibility-tolerant. Turning warnings into load failure could reject intentional cross-device vendor overlays.
- No policy/config edits and no process-group lifecycle rewrite. A1's reclaim-before-removal behavior was not duplicated.
- No micro-optimizations to profile map lookup: heterogeneous `std::map` lookup is allocation-free and profiles are already resolved inside aggregates.

## Cache and lifetime decisions

- `TaskProfiles`, `CgroupMap`, descriptors, attributes, profiles, and actions live for the process lifetime after singleton initialization.
- Cache state is per `TaskProfile` and per resource type. The new mutex covers only rare cache enable/drop transitions, not every action execution.
- `SetCgroupAction`/`WriteFileAction` continue to serialize access to shared cached FDs. This is necessary because concurrent writes through a shared file description must not interleave and cache drop must not race a write.
- App-dependent paths remain marked non-cacheable. Reopening is the safe strategy for transient UID/PID cgroups and attribute targets.
- The existing explicit `DropTaskProfilesResourceCaching()` contract remains necessary across privilege transitions so access can be reevaluated.

## Concurrency conclusions

Actions are shared through process-lifetime profiles and aggregates and can be executed from multiple threads. Immutable action fields are safe. Cached-FD actions already use per-action mutexes; A1 optional syntax state is atomic. The cpuset log-once state is now atomic. Profile cache bookkeeping is now synchronized and separated by resource type.

Configuration mutation (`Load`, `MoveTo`, attribute `Reset`) occurs only during singleton construction before publication. Runtime profile/attribute maps and aggregate graphs are read-only. No new global mutable cache was introduced.

## Capability-handling conclusions

- A1 correctly distinguishes missing `memory.reclaim`, optional swappiness syntax rejection, disappearing cgroups, partial reclaim, and genuine I/O failures; its atomic per-action syntax cache avoids repeated probes.
- Optional `SetAttribute` absence remains checked at the target path. It intentionally does not cache globally because activation and cgroup lifetime vary. Existing invalid-value and permission failures remain fatal.
- Missing controllers during config load remain warnings, allowing generic/vendor configurations to span kernels and products. Actions requiring an unresolved controller are omitted rather than becoming false-success runtime actions.
- Freezer, uclamp, cpuset, scheduler, and ordinary memory attributes are not silently made optional. Their declared action failure remains visible.
- FD-cache `FDS_INACCESSIBLE` behavior was retained because it supports the documented cache-before-privilege-drop contract; uncached opens continue to report failures.
- `cgroup.kill` and memcg availability already use process-lifetime capability checks. Their underlying platform capability is stable after early cgroup setup; no additional probing cache was added.

## Aggregate-profile conclusions

Aggregates resolve names once during loading and retain ordered shared references. Duplicates are preserved. Direct self-reference and unresolved references are rejected. More complex cycles cannot be constructed by the loader's backward-only resolved references: an aggregate can only point to an already published profile, while ordinary overlays replace actions with non-aggregate actions in the same referenced object and aggregate overlays replace the map entry with a new object. Runtime recursive traversal is therefore over a DAG.

The generic JSON has no duplicate profile/aggregate names and no direct self-reference. Aggregate execution now reports child failures while still attempting subsequent profiles in declared order.

## Error and logging conclusions

- Genuine write, scheduler, permission, malformed-state, and controller failures remain diagnosable.
- Aggregate/profile wrapper logging remains secondary to action-level detail and is verbose/warning as before.
- `SetUserProfiles` now uses `LOG`, not stale-errno `PLOG`, and identifies user profiles correctly.
- Repeated cpuset `ENOSPC` logs remain suppressed after the first occurrence, but failures are no longer converted to success.
- Optional capability absence is quiet or low severity only where explicitly optional; no global suppression was added.

## JSON/config validation

All task-profile and cgroup JSON files under `libprocessgroup/profiles` parse successfully with `jq`. The main generic task-profile database has no duplicate profile/aggregate names and no direct aggregate self-reference. Existing protobuf schema tests cover required fields, while runtime loading resolves controllers, attributes, action parameters, and aggregate references.

The loader remains deliberately compatibility-tolerant for unknown actions/controllers and vendor/system_ext overlays. Stronger transactional loading, duplicate diagnostics within an individual overlay, and richer aggregate schema tests would require a separately scoped compatibility decision.

## Process-group lifecycle conclusions

Creation establishes UID and PID cgroups, activates v2 controllers at the UID level, then moves the initial PID. Init waits for activation before child profile application. Kill uses `cgroup.kill` when available or enumerates `cgroup.procs`, waits on `cgroup.events`, performs A1 reclaim when requested, and removes v2 plus applicable v1 memcg paths. Dynamic paths are not FD-cached by task profiles, avoiding stale references across removal/recreation.

No direct A3 defect requiring lifecycle code changes was found. Existing PID reuse/stale-cgroup races are bounded by the cgroup membership files, event polling, retry deadline, and idempotent removal behavior; eliminating them completely would require kernel identity primitives beyond this task-profile scope.

## Existing Android 17 abstractions

`UClampLatencySensitive`, `CpuPolicySpread`, `CpuPolicyPack`, foreground/top-app/background/restricted scheduler and cpuset aggregates, and memory profiles all use the same generic attribute/join/aggregate mechanisms. The audit changed no values or assignments. Uclamp and freezer controls remain strict unless a configuration explicitly marks an attribute optional.

## Tests

Focused additions in `task_profiles_test.cpp`:

- concurrent repeated enable of task and process caches initializes each exactly once;
- dropping one cache type does not suppress dropping the other;
- an aggregate reports failure and still executes the next ordered child;
- process-wide `WriteFileAction` reports per-thread write failure;
- a normal scheduler policy without `Nice` safely reports syscall failure.

Existing A1 tests continue to cover optional reclaim absence, cached syntax support, genuine write failure, and missing cgroup state.

## Validation

- `git diff --check`: pass.
- `jq empty libprocessgroup/profiles/*.json`: pass for every JSON file.
- Duplicate-name query on generic `task_profiles.json`: no duplicates.
- Direct aggregate self-reference query: none.
- Focused Android test/build execution: unavailable in this checkout. It contains only the `system/core` repository and has no Android build tree, `soong_ui.bash`, `m`, `atest`, generated headers, or prebuilt `task_profiles_test` binary.
- Manual final diff review: no JSON/policy changes, no device-specific paths or values, no Power HAL/device-tree changes, no global cache, and no cached dynamic cgroup FDs.

## Remaining limitations

- The loader logs and skips several malformed/unsupported actions rather than failing the entire file; this is longstanding overlay compatibility behavior.
- `SetAttribute` optional absence is evaluated per operation and therefore still incurs a failed open plus existence check when an optional attribute is unavailable. Caching that absence safely would need a hierarchy/cgroup-generation invalidation model.
- Cached stable-path FDs intentionally require explicit dropping across privilege changes; callers that violate that contract may retain the pre-drop access result.
- Process-wide thread enumeration is inherently racy with thread exit/creation. It now reports observed write failures but cannot provide an atomic all-threads guarantee.
- No on-device performance measurements were possible in the partial checkout.

## Benchmark requirements

Before/after measurements should run on a generic Android 17 target and include:

- repeated `SetTaskProfiles(..., use_fd_cache=true)` for scheduler/cpuset aggregates under 1, 4, and 16 concurrent callers;
- repeated process-profile application for static and `<uid>/<pid>` cgroup paths;
- CPU time, wall time, allocations, `openat`, `write`, `access`, and lock contention counts;
- controller path expansion microbenchmark demonstrating removal of regex construction;
- cache enable/drop stress under concurrent application (TSAN-capable host test where available);
- lifecycle stress with cgroup creation/removal and PID/thread churn to confirm no stale-FD regression.

Acceptance should require unchanged action ordering and syscall counts except for eliminated regex work/corrected failure paths, no throughput regression outside noise, and no new contention in ordinary cached execution.

## Recommended commit subject

`libprocessgroup: harden task profile execution and caching`

## Recommended commit body

```
Keep task and process resource-cache state independent and synchronize cache
enable/drop transitions because TaskProfile instances are shared across
threads. Preserve the existing action-level FD locking and avoid caching
dynamic application cgroup paths.

Propagate failures from aggregate, UID, and process-wide WriteFile actions,
while preserving aggregate execution order. Fix scheduler failure logging
without an optional nice value and keep repeated cpuset ENOSPC failures from
being misreported as success.

Replace per-call regex construction for literal UID/PID path substitutions
and add focused concurrency and failure-propagation tests. No task-profile
policy or device-specific configuration is changed.
```

## Git status --short

```
 M libprocessgroup/cgroup_map.cpp
 M libprocessgroup/task_profiles.cpp
 M libprocessgroup/task_profiles.h
 M libprocessgroup/task_profiles_test.cpp
?? .codex-reference/
```
