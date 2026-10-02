# Android 17 generic init critical-path audit (A4)

Date: 2026-10-02

## Executive conclusion

No init code change is justified by this source audit alone.

The generic startup path is intentionally serialized where it establishes kernel state, SELinux state, the property namespace, mount namespaces, rc ordering, or explicit service dependencies. The expensive-looking generic `init.rc` edges are dependency barriers rather than accidental sleeps. Existing code also already avoids several obvious hazards: uevent coldboot runs in another process, property service work runs on dedicated threads, persistent writes can use a worker, readahead forks, service children wait for parent-side cgroup setup without stalling unrelated init work, and updatable services are delayed until the correct APEX namespace is ready.

Two concrete scalability candidates were found but not changed without device evidence:

1. `ActionManager::ExecuteOneCommand()` linearly scans every action for each queued event/property change.
2. `HandleProcessActions()` linearly scans every service on each main-loop pass.

Both are real O(N) mechanisms, but changing either would add indexing/state-maintenance complexity to ordering-sensitive code. No trace, benchmark, or failing test in this checkout establishes them as material Android 17 boot bottlenecks. Likewise, per-service executable `stat()` and SELinux transition computation repeat on service restarts, but caching would risk stale executable/label semantics across APEX activation, remount, policy transitions, and updates.

The most useful next step is a device trace/measurement pass, not speculative restructuring. There is an observability gap before `early-init`: second-stage property initialization, label-handle initialization, namespace setup, subcontext creation, and aggregate boot-script parsing do not have durable individual timings. Per-file parser VERBOSE logs and aggregate stage timestamps exist, but bootchart and tracefs are only enabled by rc actions after those phases. If measurements show this interval is material, the smallest follow-up should add coarse, feature-gated or thresholded `boot_clock` attribution around those existing phase boundaries; it should not add per-token/per-property tracing.

## Scope and inspected sources

The audit covered the requested startup implementation: `main.cpp`, `init.cpp`, `first_stage_init.cpp`, `second_stage_resources.h`, `service.cpp/.h`, `service_parser.cpp`, `action.cpp`, `action_manager.cpp`, `parser.cpp`, `property_service.cpp`, `property_type.cpp`, `subcontext.cpp`, `mount_namespace.cpp`, `builtins.cpp`, `util.cpp`, `epoll.cpp`, `reboot.cpp`, `selinux.cpp`, `persistent_properties.cpp`, and `Android.bp`.

Related files inspected for control flow and tests included `action_parser.cpp`, `import_parser.cpp`, `service_list.cpp`, `sigchld_handler.cpp`, `bootchart.cpp`, coldboot/uevent code, tokenizer/parser tests, init/action tests, property tests, service tests, persistent-property tests, epoll tests, reboot tests, subcontext tests, and `init/TEST_MAPPING`. Generic rc review covered `rootdir/init.rc`, `init-perfetto.rc`, debug, USB, zygote, BoringSSL, and ASan rc files. There are no `init/*.rc` files at the top of this checkout; the prefetch rc is under `init/libprefetch/prefetch/` and is not part of the core generic init sequencing reviewed here.

`reboot.cpp` is primarily the shutdown/reboot critical path, not normal startup. It was checked for startup-facing service calls and wait behavior; no A4 startup change was indicated.

## Startup architecture map

### 1. PID 1 dispatch and first stage

`main.cpp` dispatches by executable/argument to ueventd, a vendor subcontext, SELinux setup, second stage, or `FirstStageMain()`.

`FirstStageMain()` executes synchronously as PID 1:

1. Clears the environment/umask, sets `PATH`, mounts the minimal `/dev`, `/proc`, `/sys`, selinuxfs, `/mnt`, debug ramdisk, and second-stage resource tmpfs trees, and creates essential device nodes.
2. Reads kernel cmdline and bootconfig, establishes logging, and handles the optional first-stage console.
3. Loads kernel modules. Supported module parallelism is selected by boot configuration and implemented below this caller; init waits for completion because subsequent device discovery/mounts depend on it. A module duration is exported for second-stage reporting.
4. Optionally resumes hibernation and preserves ramdisk/debug resources.
5. Builds required block devices and performs first-stage mounts/verity through `FirstStageMount`. This is synchronous and boot-critical.
6. Frees the old ramdisk where applicable, exports the first-stage start timestamp, and `execv()`s `/system/bin/init selinux_setup`.

### 2. SELinux setup

`SetupSelinux()` synchronously:

1. Initializes kernel logging and optional debug overlay preparation.
2. Mounts missing system partitions if needed and reads the split/precompiled policy.
3. Coordinates the snapuserd transition where snapshots require it.
4. Loads policy, enables the selected enforcement mode, performs security-critical restorecons, records the SELinux phase start timestamp, transitions from the kernel domain, and `execv()`s second stage.

This ordering is security- and availability-critical. Policy, snapuserd, enforcement, labeling of init, and domain transition cannot be deferred or parallelized safely.

### 3. Second-stage process setup

`SecondStageMain()` synchronously initializes PID 1 state before rc execution:

1. Installs shutdown handlers where configured, logging, signal behavior, PID 1 OOM adjustment, and `/dev/.booting`.
2. Runs `PropertyInit()` before any rc conditional evaluation. It creates serialized property-info data, initializes the property area, loads property contexts, imports device-tree/bootconfig/cmdline boot properties, invokes the vendor boot-environment hook, exports kernel properties, and loads boot/default/derived properties.
3. Unmounts temporary resources, mounts `/apex`/`/linkerconfig` scaffolding, initializes SELinux label handles, and restores init-created paths.
4. Creates the main epoll instance, installs child-reaping/signalfd/notifier handlers, and starts two property-service threads plus an optional persistent-write thread.
5. Records first-stage and SELinux timings, handles overlays/GSI state, determines USB behavior, creates mount namespaces, and forks/execs the vendor init subcontext.
6. Parses boot scripts synchronously and deterministically, constructing services/actions in file/import order.
7. Queues builtin and named events (`early-init`, coldboot wait, `init`, then `late-init` or `charger`, and initial property-trigger evaluation), restores init priority, and enters the epoll-driven main loop.

USB controller discovery is normally initiated without waiting. In non-normal boot with explicit `ro.boot.wait_for_udc`, second stage creates a helper thread and joins it with the existing bounded wait. That is opt-in boot-mode policy, not a generic normal-boot stall.

### 4. Rc parsing and object construction

`LoadBootScripts()` parses the primary hardware `init.rc`, then sorted regular files in the system, system_ext, vendor, odm, and product init directories. Imports are expanded while parsing and processed by `ImportParser::EndFile()`, preserving textual/file completion order. Missing legacy partition directories are saved for `import_late` after `mount_all`.

The tokenizer operates once over each loaded file buffer. A section parser creates `Action` and `Service` objects; command argument vectors retain unexpanded properties until invocation where required. Directory entries are sorted before parsing, providing deterministic ordering. No parser-level parallelism is safe without recreating exactly the same import, override, action, and error order.

No global visited-file set exists, so explicitly importing the same file more than once can repeat parsing. That is not automatically redundant: repeated imports historically participate in service duplicate/override handling and can append duplicate actions. Deduplicating them would change rc semantics and was rejected.

### 5. Action queue and main loop

Events are queued under a mutex as event strings, property name/value pairs, or direct builtin-action pointers. When no action is currently executing, `ActionManager` removes one event at a time and scans actions in insertion order, enqueueing each match in that same order. It then executes exactly one command per main-loop turn. This preserves action and event ordering, lets epoll callbacks/child reaping interleave between commands, and prevents a long action list from monopolizing PID 1 beyond a single builtin.

Property actions match only actions without an event trigger. The changed property is checked first, then remaining conjunctive property conditions are read from the property area. The initial empty property event evaluates all property-only actions against current state. There is no duplicate evaluation within a single action/event beyond the required conjunct checks.

While an `exec` service or explicit property wait is active, the main loop intentionally stops executing further rc commands. Epoll remains active, so child exits, properties, and control messages continue to be handled.

### 6. Generic rc trigger chain and service classes

The generic normal-boot chain is:

`early-init` -> `init` -> `late-init` -> `early-fs` -> `fs` -> `post-fs` -> `late-fs` -> `post-fs-data` -> `load-bpf-programs` -> `zygote-start` -> `firmware_mounts_complete` -> `early-boot` -> `boot`.

`early-init` starts ueventd and synchronously waits for `init_dev_config` and bootstrap apexd because their effects are prerequisites. The queued coldboot builtin later pauses rc command execution until ueventd publishes completion. `late-fs` starts `early_hal`. `boot` starts `hal` and `core`. The `nonencrypted` event starts `main` and `late_start`. Zygote starts only after odsign verification. These are explicit dependency/policy edges, not hidden implementation serialization.

Property changes from the property threads enqueue work and wake PID 1 using nonblocking `eventfd`. At boot completion, generic init stops bootchart, recreates `/data/per_boot`, and responds to subsequent property-triggered maintenance. Framework/service work after service start is asynchronous to init unless rc explicitly uses `exec`, `exec_start`, or `wait_for_prop`.

## Synchronous versus asynchronous work

| Classification | Work | Context | Does init block? | Ordering/dependency conclusion |
|---|---|---|---|---|
| A: unavoidable synchronous | Minimal mounts/nodes, module completion, device creation, verity/first-stage mounts | PID 1 first stage | Yes | Needed to access the executable, policy, and partitions for later stages. |
| A | Policy read/load, snapuserd transition, enforcement, critical restorecon/domain transition | PID 1 SELinux setup | Yes | Security and snapshot I/O ordering are mandatory. |
| A | Property-info creation, property-area initialization, boot/default properties | PID 1 second stage | Yes | Rc imports/conditions, service definitions, SELinux property checks, and startup decisions consume them. |
| A | Label initialization/restorecon, epoll/signal/property sockets, mount namespaces, rc parsing | PID 1 second stage | Yes | Required before safely executing vendor commands or starting services. |
| A | Parent half of `Service::Start()` through fork/clone, cgroup creation/profile setup, state publication | PID 1 main loop | Briefly | Child is held on a FIFO until cgroup setup is complete; publishing `running` earlier would be incorrect. Console services additionally synchronize `setsid()`. |
| B: required, already asynchronous | uevent coldboot/device handling | ueventd process/thread-pool implementation | Only at the explicit coldboot barrier | Work overlaps early rc activity; consumers of `/dev` require completion. |
| B | Property request handling | Two property-service threads | No for ordinary requests | PID 1 receives callbacks/wakeups; SELinux/type checks stay in property service. |
| B | Persistent writes when configured | property writer thread | No for the disk write, requester completion is delayed | Configuration preserves compatibility where strict ordering is required. |
| B | Service child namespace/env/fd/attribute setup and exec | Forked service child | Parent waits only for required handshake | Independent child work proceeds after cgroup release. |
| B | `readahead` builtin | Forked low-priority child | No | Explicitly designed not to block PID 1. |
| C: deferred | Persistent-property load | Property thread, requested by `load_persist_props` after `/data` | Rc intentionally waits for ready property | Cannot move beyond consumers of persistent-property triggers. |
| C | Main/late service classes and boot-complete cleanup | Services/property actions | No except commands explicitly marked blocking | Deferred by existing trigger structure. |
| D: optional/debug | Bootchart, boot monitor, debug ramdisk/overlay handling, state dump, debug trace instance | Conditional worker/debug paths | Bootchart stop joins its worker; other effects are conditional | Disabled or conditional in production; no always-on verbose dump was found. |
| E: serialized by design | `exec`, `exec_start`, `wait`, `wait_for_prop`, restorecon, mount, copy, mkdir/chown/chmod, action commands | PID 1 or exec child | Yes where documented | Rc authors request these barriers; reordering would change visible filesystem/property/service state. |
| F: potentially repeated | Action scan/event, service scan/main-loop, executable stat/context on restart, property conjunct lookups | PID 1 | Adds CPU work, not I/O wait in most cases | Concrete repeated mechanisms, but no evidence justifies semantic-risky caching/indexing. |

## Existing timing and tracing mechanisms

Existing generic visibility includes:

- `ro.boottime.init`, `ro.boottime.init.first_stage`, `ro.boottime.init.selinux`, module duration, coldboot wait, and `mount_all` duration properties.
- Per-service first-running timestamps in `ro.boottime.<service>` and optional first-execution timestamps for named init events behind the `enable_init_event_timestamp` flag. Tests cover disabled/enabled behavior, execution-time semantics, chained triggers, and first occurrence.
- Every builtin command is timed; failures, commands over 50 ms, and all commands at debug log level receive file, line, trigger, command, duration, and result attribution.
- `exec` and background oneshot completion logs include runtime. File waits and property waits report elapsed time. Coldboot and selected device/firmware operations are timed.
- Parser file reads/parses and property-file loads have VERBOSE duration logs. Parser INFO logs identify files/directories and action INFO logs identify their source line.
- `mount_all` publishes duration by phase (`early`, `default`, `late`).
- Bootchart samples process/stat/disk data on a worker thread when explicitly enabled. The early location can capture from the `early-init` action onward; data-backed bootchart begins after `/data` is available.
- Tracefs is mounted by `early-init`, and Perfetto-related generic rc prepares kernel tracing/perf access. Init itself does not currently place scoped atrace slices around its pre-rc phases.
- Kernel boot timestamps are carried through `boot_clock` values and exported for bootstat rather than inferred from wall time.

This is sufficient to attribute slow rc commands, explicit waits, service launch points, first-stage/SELinux aggregate time, coldboot, and mount-all. It is not sufficient to separate the pre-`early-init` second-stage interval into property initialization, label setup, namespace setup, subcontext startup, and aggregate parsing on a production device without adding temporary instrumentation or collecting lower-level scheduler/I/O data.

No instrumentation was added. Always-on logs would add boot noise and formatting/I/O overhead. New read-only properties would consume stable property namespace and require defining lifecycle/units. Scoped atrace is not useful for the earliest phases until tracefs/tracing is available. A follow-up should first prove the opaque interval is important, then add a small number of coarse thresholded timings or an existing flag-gated metric with tests.

## Hotspots and repeated-work findings

### Rc/parser

- Each file is read and tokenized once per import occurrence. Directory scans consider regular entries and sort full paths, preserving deterministic order.
- Service/action construction necessarily allocates retained strings and command vectors. Properties in commands are expanded at execution because values may change; pre-expanding them would be incorrect.
- Trigger parsing uses a small ordered map for conjunctive property triggers. This provides deterministic trigger-string construction and rejects duplicate property keys.
- Imports are processed after the containing file reaches `EndFile`; changing this or parsing in parallel would alter ordering and error/override behavior.
- INFO logging occurs per file/directory and per action start. This is bounded by configuration size, carries useful attribution, and is not a high-frequency polling-loop log. Per-file durations remain VERBOSE.

Conclusion: no duplicate tokenization or immutable computation was proved accidental. Do not add import deduplication or parser parallelism.

### Trigger/action handling

- Event/property dispatch scans `actions_` linearly. Current storage does not index event names or property names.
- The scan is also the source of exact insertion-order execution. A safe index would need to maintain a globally ordered candidate sequence across named events, changed-property candidates, initial all-property evaluation, dynamically imported actions, APEX action removal, oneshot deletion, and builtin pointer events.
- Each property-triggered candidate avoids reading all properties unless the changed name/value matches. Remaining conjuncts must be re-read because property state is mutable.
- `Action::ExecuteOneCommand()` copies the selected `Command` deliberately: executing an import may mutate the action command vector and invalidate references.
- Trigger and command strings are built for action-start logging and for slow/failing/debug command logs, not continuously while idle.

Conclusion: indexing is a valid benchmark candidate, not a justified A4 implementation. Preserve scan/order semantics until measured action count, queue count, and CPU samples demonstrate material cost.

### Property service

- Property security initialization is synchronous and correctly precedes rc parsing: serialized property contexts, property area creation, property-info loading, boot sources, vendor hook, default and derived properties.
- Property socket setup and request loops move to dedicated threads before rc actions execute. Callbacks enqueue property events and wake PID 1 without a blocking notification socket.
- Persistent properties are intentionally delayed until `/data` exists, loaded by the property thread, then signal `ro.persistent_properties.ready`; init's `load_persist_props` builtin installs an explicit barrier because subsequent property triggers depend on the complete snapshot.
- Persistent-property recovery/legacy migration does extra disk I/O only on failure/migration paths. Staged `next_boot` processing is a necessary one-pass transformation.
- SELinux property permission checks, property type checks, legal-name/value checks, and socket peer handling are security/correctness work and must not be cached away or deferred.
- Optional async persistent writes already exist. Changing its default would alter requester completion/durability ordering and is outside this audit.

Conclusion: no duplicated validation or safely deferrable first-stage property work was found.

### SELinux

- Split/precompiled policy discovery and compilation/loading occur before enforcement/domain transition. Snapshot-aware policy loading has explicit snapuserd barriers.
- `SelabelInitialize()` in second stage and in the vendor subcontext gives each process the label handles it requires. This is not redundant shared work because the processes have separate address spaces and security domains.
- Early restorecons cover init-created tmpfs/device paths and the init executable; rc restorecons cover filesystems and directories only after they exist/mount. Apparently repeated `/metadata` and `/metadata/apex` labeling occurs at different lifecycle points or documents compatibility repair. Removing it without filesystem/policy evidence could create security regressions.
- Property, service, and file context boundaries are preserved. Vendor/odm commands run through the vendor subcontext; actionable property checks enforce the platform/vendor boundary.

Conclusion: no obviously redundant generic security work is safe to remove. Security ordering takes precedence.

### Service start

`Service::Start()` performs these parent-side steps once per actual start: updatable-service namespace readiness check; state reset; two FIFO creations; console validation; executable property expansion and `stat`; SELinux transition-context lookup; permanent selection of bootstrap/default namespace; socket/file descriptor creation; fork/clone; OOM/cgroup setup; normal I/O profile and optional memory controls; LMKD registration; child release; process-group/session synchronization; state-property publication.

The child performs namespace entry, environment setup, descriptor publication, pid-file writes, waits for parent cgroups, applies task profiles, sets IDs/capabilities/context/priority/session state, expands remaining arguments, closes/duplicates descriptors through the execution path, and execs.

Findings:

- The parent does not wait for exec completion for normal services. `exec`/`exec_start` intentionally set a global barrier until SIGCHLD.
- The cgroup FIFO is required: task profiles and service code must not race before cgroup creation/controller activation. The separate session FIFO prevents cross-direction notification races.
- Executable `stat()` and automatic SELinux context computation repeat after stop/restart. Caching is unsafe without invalidation across filesystem/APEX/policy changes; restarts are not the dominant initial-boot service-create path by assumption.
- Only the executable argument is expanded and stored before start; other args expand immediately before exec so dynamic property semantics remain correct.
- Sockets/files must be recreated per start because ownership, labels, lifecycle, and descriptors are start-specific.
- `ServiceList::FindService` is linear (as are PID lookups). Names are unique and lists are modest, but this can be benchmarked if profiles identify it. Introducing a second ownership/index container solely on speculation was rejected.

Conclusion: no safe immutable precomputation or false parent wait was identified.

### Builtins, filesystem, and mounts

- `mount`, `mount_all`, restorecon, copy, write, mkdir, chmod/chown, symlink, and explicit wait builtins run synchronously in action order. The kernel/filesystem results are normally prerequisites for the next command.
- `mount ... wait` and `wait` poll only when rc explicitly requests existence. Wait functions report elapsed time and retain timeouts/error propagation.
- `mount_all` reads the selected/default fstab once per invocation, invokes fs_mgr once, records duration, performs required late imports, then emits the filesystem result event. Different early/default/late invocations are different policy phases, not duplicate calls.
- Mount namespace creation is one-time. Namespace IDs/fds are cached. Per-child namespace switching reads the current namespace ID so services with their own namespace are not incorrectly moved.
- `readahead` already forks to avoid blocking init. `exec_background` similarly avoids the global exec barrier where rc semantics allow it.
- Restorecon loops and recursive walks are inherently synchronous when subsequent services require labels. Safety/error checks are not redundant.

Conclusion: no system/core caller was shown duplicating fs_mgr work, mount table parsing, namespace creation, or canonicalization.

## Blocking edges found

The important generic edges and their consumers are:

- Kernel modules/device discovery -> first-stage mounts: required for block devices and filesystems.
- First-stage mounts -> policy read -> enforcement/domain transition: required for policy availability and security.
- Property initialization -> rc parsing: imports, triggers, service arguments, compatibility gates, and security contexts consume properties.
- `early-init` `exec_start init_dev_config` -> bootstrap apexd: device configuration can select bootstrap APEX behavior.
- `exec_start apexd-bootstrap` -> `perform_apex_config`/later service starts: critical libraries/configs must exist.
- Coldboot property -> commands requiring `/dev`: explicit required barrier; uevent work overlaps preceding actions.
- `post-fs` checkpoint `vdc` -> remount/data progression: storage consistency policy.
- `post-fs-data` checkpoint/keymaster commands -> encrypted data/APEX/keystore users: explicit storage/security dependencies.
- `load_persist_props` -> initial persistent property actions: complete property state is required.
- Casefold remover -> data mirror/storage setup: consumer requires adjusted `/data/media` state.
- `apexd.status=activated` and KeyMint module hash -> code from updatable APEXes: module availability/measurement dependency.
- mainline aconfig, derive SDK/classpath, ART boot -> odsign: generated configuration and classpath/ART properties are inputs.
- odsign key completion -> boot-level closure; odsign verification -> zygote: signing/security correctness.
- `class_start` iterates services synchronously but each successful normal `Start()` returns after the parent handshake, not after service initialization. Cross-service readiness is property/socket/service-manager driven.

Edges whose placement/content belongs to product/device rc cannot be changed generically. No device-owned action was reordered.

## Logging and debug conclusions

- Action-start INFO logs and service start/exit logs are event-bounded and essential for diagnosis.
- Slow/failing command formatting is conditional; ordinary command strings are not built for production INFO unless the command crosses 50 ms or fails.
- Parser announces each input at INFO but only formats timings at VERBOSE. This is finite startup work, not a loop.
- Bootchart is disabled unless a boot property or enable file requests it and runs in a worker. State dumping is hard-disabled because its discarded INFO work was already measured as costly.
- Debug boot monitor, debug ramdisk/overlay behavior, debug trace instance, and userdebug I/O statistics are property/build conditional.
- No repeated production property dump or diagnostic filesystem read in an idle/hot polling path was found.

No logs were removed or added.

## Allocation and string conclusions

- Rc tokens, action commands, service options, and trigger maps are retained configuration and allocated once per parse occurrence.
- Runtime command arguments are copied/expanded for mutation safety and dynamic-property correctness.
- Event queue entries own strings because producers include property threads and lifetime cannot rely on caller buffers.
- Service environments and descriptor vectors are constructed per start because one-shot environment entries, sockets, files, and FDs change by start.
- `BuildTriggersString()` and `BuildCommandString()` allocate only for attribution paths.
- Existing static caches are used for immutable decisions such as mount-namespace count and vendor API behavior. Broader caching was not justified.

## Concurrency conclusions

Potential parallel work was considered conservatively:

- Parsing partition directories in parallel fails the deterministic-order proof: imports, duplicate/override services, action insertion order, errors, and vendor subcontext classification are shared mutable outcomes.
- Parallel action execution fails immediately because rc ordering is the programming model and builtins mutate shared kernel/filesystem/property/service state.
- Property initialization components cannot be split safely without proving ordering among property contexts, area creation, boot properties, vendor hook, and default/derived property expansion.
- SELinux policy and restorecon work cannot cross enforcement/domain boundaries.
- Independent directory creation/chown operations in `init.rc` might look parallel, but adding a scheduler and completion barrier would add nondeterministic failure/log ordering and likely cost more than these syscalls. This is rc policy, not a structural defect.
- Existing concurrency is retained where proof exists: ueventd/coldboot, property threads, optional persistent writer, service children, readahead child, bootchart worker, and bounded USB discovery.

No new concurrency was implemented.

## Changes made

No source, build, rc, or test code was changed. This report is the only new A4 artifact.

## Changes deliberately rejected

- Event/property trigger indexes: measurable complexity candidate, but no device evidence and high ordering/update risk.
- Service-name/PID indexes: no demonstrated boot cost; additional synchronization/invalidation surface.
- Import deduplication: changes established repeated-import semantics.
- Parser parallelization: cannot prove identical deterministic ordering.
- Pre-expansion of rc arguments: properties are mutable and execution-time expansion is intentional.
- Caching executable `stat` or SELinux service context: stale across restarts, APEX/filesystem changes, and policy lifecycle.
- Deferring property info/default loading: breaks rc conditionals, service configuration, and security checks.
- Removing restorecons: lifecycle/security purpose differs even where paths overlap.
- Making `exec_start` or `wait_for_prop` nonblocking: breaks explicit dependency semantics.
- Reordering generic `post-fs-data` operations: dependencies are documented and cross component/security boundaries.
- Always-on fine-grained logs/traces: unproven need, boot overhead/noise, and earliest tracefs availability limitation.
- Any device policy, priority, scheduler, cpuset, frequency, storage, Power HAL, watchdog, crash, pstore, panic, or SELinux behavior change: outside scope.

## Tests and validation

Because no implementation changed, no new tests were added and target/device tests were not run. Existing focused coverage was inspected:

- `init_test`: event matching/order, property-trigger behavior, duplicate and override services, event timestamp flag/ordering/first occurrence, parser/action execution.
- tokenizer/parser tests: lexical behavior and deterministic parsing primitives.
- service tests: service parsing/start-related state and descriptor behavior.
- property, property-type, and persistent-property tests: validation, socket behavior, serialization/recovery.
- epoll, subcontext, reboot, and utility tests for their respective mechanisms.
- `init/TEST_MAPPING`: `CtsInitTestCases`, `init_kill_services_test`, and `MicrodroidHostTestCases` presubmit coverage.

Validation performed:

- Source/rc static audit and targeted symbol/call-site searches.
- Confirmed no diff under `init/` or `rootdir/`.
- `git diff --check` (passed; report included).
- Final `git status --short` recorded below.

## Remaining limitations

- This was a source audit in a system/core checkout, not a measured device boot. It cannot quantify action count, property event volume, storage latency, SELinux policy load cost, restorecon inode count, service fork/exec latency, or scheduler contention.
- Product/vendor/APEX rc files present only in a full product build determine much of the real action graph and can dominate the generic path.
- Kernel configuration, filesystem state, update/migration state, encryption, snapshots, APEX layout, and build type materially change timings.
- Per-file VERBOSE timers are not generally available as structured production metrics. The pre-`early-init` second-stage subphases remain coarsely attributed.
- Static review cannot prove that the O(actions) dispatch or O(services) process scan is insignificant on products with unusually large rc graphs.

## Benchmark and trace requirements

Before changing generic structures, collect repeated cold and warm boots on representative user builds, including normal and post-OTA/snapshot cases:

1. Capture kernel/init logs and all `ro.boottime.init*`, `ro.boottime.event.*` (with the existing flag in a controlled build), `ro.boottime.<service>`, and `ro.boottime.init.mount_all.*` values.
2. Enable bootchart only for measurement and collect a Perfetto/ftrace boot trace beginning as early as the platform permits, with sched, process, block I/O, binder, and filesystem events appropriate to the device.
3. Instrument a controlled build with coarse `boot_clock` boundaries around `PropertyInit`, `SelabelInitialize`/`SelinuxRestoreContext`, `SetupMountNamespaces`, `InitializeSubcontext`, and `LoadBootScripts`; report only thresholded or structured results.
4. Record action count, event/property queue count, actions examined per event, property conjunct reads, service count, and `HandleProcessActions` scan frequency using test-only counters or a benchmark build—not production verbose logs.
5. Attribute every explicit `exec`/property wait using existing logs and correlate its producer process. Separate init overhead from required external component latency.
6. Compare distributions, not single boots, and verify identical rc/action/service ordering and failure outcomes for any proposed patch.

An action index should only proceed if CPU samples/counters show dispatch is material. A service index should only proceed if lookup/process scans are material. Coarse observability should only be upstreamed if the pre-rc interval is both significant and otherwise unattributable.

## Recommended next step

Run the measurement plan on representative devices/products. The first decision point is whether time is in pre-rc second-stage setup, explicit rc dependency waits, external services, or action/service scans. If pre-rc setup is opaque and material, prepare a narrowly scoped observability patch with coarse phase timers and focused tests. Otherwise optimize the measured component rather than generic init.

## Recommended commit subject/body

Not applicable: no code changed. The report should not be committed as an init runtime change unless project policy explicitly tracks audit artifacts.

## Git status --short

Final status is recorded after validation in the handoff output. The `.codex-reference/` directory was already untracked at audit start and contains prior A-series artifacts; this audit adds `a17-init-critical-path-audit.md` inside it. No files were staged, committed, or pushed.
