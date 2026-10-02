# LOSP Android 17 system/core performance tuning audit

## Scope and conclusion

This report audits the two historical Android 16 Purgatory-derived patches:

- `0001-init-Add-system-performance-optimizations-from-Purga.patch`
- `0001-init-Update-system-tuning-parameters-Purgatory-2.3-b.patch`

against the current LOSP Android 17 `system/core` source and designs an A17-native implementation strategy. The patches are historical design input, not a porting baseline.

Neither patch should be applied wholesale. Most proposed writes are device-specific, tied to old or optional kernel interfaces, redundant with current A17 behavior, or weaken reliability and diagnostics for little measurable benefit. Several commands in the patches also appear outside an `on` action after an `import`; those forms are invalid Android init syntax.

The recommended A17 design is:

1. Keep generic `system/core` conservative and mechanism-oriented.
2. Use task profiles, cgroup abstractions, PSI, `memory.reclaim`, MMD, and fs_mgr aliases instead of raw global sysfs/sysctl writes.
3. Put CPU topology, scheduler extensions, storage policy, devfreq, networking, and driver policy in Pixel/device/kernel configuration.
4. Use Pixel libperfmgr/Power HAL for bounded interaction, launch, uclamp, PM QoS, GPU, and interconnect requests.
5. Preserve pstore, watchdogs, crash dumps, tracefs, Perfetto, panic recovery, and field-debugging evidence.
6. Require controlled A/B measurement before adopting performance values.

No immediate generic `rootdir/init.rc` tuning commit is justified by these patches.

## Current A17 architecture

The current tree already contains the principal mechanisms needed for a cleaner design:

- `rootdir/init.rc` mounts tracefs directly at `/sys/kernel/tracing`.
- `rootdir/init-debug.rc` controls debugfs conditionally and normally unmounts it after boot.
- pstore is mounted and consumed for boot-reason, rollback, panic, and filesystem-integrity diagnosis.
- task profiles expose `UClampMin`, `UClampMax`, and `UClampLatencySensitive`.
- `CpuPolicySpread` and `CpuPolicyPack` already toggle latency-sensitive placement.
- foreground, foreground-window, and top-app aggregates already combine CPU controller placement, cpuset placement, I/O priority, and timer slack.
- AOSP explicitly leaves actual cpuset CPU masks to device configuration.
- PSI memory pressure is exposed for lmkd.
- libprocessgroup implements cgroup-v2 `memory.reclaim`, including capability checks for optional swappiness syntax.
- AConfig controls reclaim-before-memcg-removal rollout.
- `/dev/sys/fs/by-name/userdata`, `/dev/sys/block/by-name/userdata`, and `/dev/sys/block/by-name/rootdisk` avoid hard-coded block names.
- F2FS iostat is enabled only on debuggable builds.
- the tree contains property-gated boot prefetch record/replay infrastructure.
- LOSP already disables the bootreceiver tracing instance by default, saving its documented memory allocation while preserving an opt-in path.

## Classification key

- **A**: valid and broadly applicable on A17
- **B**: useful idea, but device/vendor-specific
- **C**: already implemented or superseded upstream
- **D**: obsolete, missing, or optional kernel interface
- **E**: device/kernel-specific and unsafe globally
- **F**: weakens diagnostics, reliability, recoverability, functionality, or security
- **G**: redesign using current A17 mechanisms

Recommendations are `KEEP`, `ADAPT`, `DEVICE-SPECIFIC`, `DROP`, `ALREADY-UPSTREAM`, or `NEEDS-BENCHMARKING`.

## Historical change audit

### Block I/O and F2FS

| Setting/change | A16 value | Current A17 behavior/default | Interface exists? | Scope | Expected purpose and plausible benefit | Downside/risk | Class | Recommendation |
|---|---:|---|---|---|---|---|---|---|
| mq-deadline `fifo_batch` on `nullb0`, `sda`-`sde` | 8 | No generic override | Only when the device uses mq-deadline | Storage-specific | Smaller batches may reduce tail latency | Lower throughput; absent nodes; wrong scheduler/device | B/E | DEVICE-SPECIFIC; benchmark |
| mq-deadline `front_merges` | 0 | Scheduler default | Same | Storage/workload-specific | Avoid front-merge lookup work | Lower merge quality; negligible benefit on some flash devices | B/E | DEVICE-SPECIFIC; benchmark |
| mq-deadline `read_expire` | 100 ms | Scheduler default | Same | Storage/workload-specific | Earlier read deadline | Write disruption and lower throughput | B/E | DEVICE-SPECIFIC; benchmark |
| mq-deadline `writes_starved` | 1 | Scheduler default | Same | Storage/workload-specific | Reduce write starvation | Can worsen foreground read latency | B/E | DEVICE-SPECIFIC; benchmark |
| `/sys/block/sde/device/delete` | 1 | No equivalent | SCSI only | Exact device | None as a normal tuning | Removes a live device; potential data loss/boot failure | E/F | DROP |
| F2FS `gc_urgent_sleep_time` | 5 ms | A17 writes 50 through userdata alias | F2FS/version-dependent | Storage-specific | More aggressive urgent GC | High I/O, power, wear, and foreground jank | B/E | DEVICE-SPECIFIC; benchmark |
| F2FS `iostat_enable` | 0 | Enabled only on `ro.debuggable=1` | F2FS-dependent | Build-conditioned | Avoid accounting on production | Patch is redundant on user builds and hurts userdebug diagnosis | C | ALREADY-UPSTREAM |
| F2FS `seq_file_ra_mul` | 128 | Already 128 through userdata alias | F2FS-dependent | Portable alias | Sequential-read throughput | Extra cache/I/O in unsuitable workloads | C | ALREADY-UPSTREAM |
| Hard-coded `/sys/fs/f2fs/dm-38`, `sda5`-`sda8` | GC sleep 5 | Runtime userdata alias | Names unstable | Device-specific | Target partitions | Wrong/missing partitions and duplicate policy | C/E | DROP |
| Discard queue targeting | Not introduced by patches | A17 already uses by-name aliases and a 128 MiB maximum | Queue-dependent | Portable alias with vendor policy | Bounds long discard stalls | May reduce discard efficiency | C | ALREADY-UPSTREAM |

Generic init cannot safely assume block enumeration or the active scheduler. If MQ-deadline tuning survives benchmarking, a device-owned helper should resolve the fstab/fs_mgr boot-device alias, verify the physical device and active scheduler, check each node, and then apply one measured profile. It must not touch `nullb0` or enumerate `sd[a-e]`.

### CPU scheduling and cpuidle

| Setting/change | A16 value | Current A17 behavior/default | Interface exists? | Scope | Benefit sought | Downside/risk | Class | Recommendation |
|---|---:|---|---|---|---|---|---|---|
| Foreground/top-app `cpu.uclamp.latency_sensitive` | 1 | Attribute and `CpuPolicySpread` already exist | Android kernel extension; not universal upstream ABI | Task/profile-specific | Prefer idle CPUs and lower wakeup latency | More migrations, active cores, and power when broad | C/G | ADAPT selectively through task profiles |
| Foreground cpuset | `0-5,7` | Inherits available CPUs; device must set masks | Yes | Topology-specific | Allow a prime core | Invalid elsewhere; unexplained exclusion of CPU 6 | E | DEVICE-SPECIFIC |
| `sched_latency_ns` | 15 ms instead of A17 10 ms | A17 explicitly uses 10 ms | Version-dependent and disappearing on newer schedulers | Kernel-wide | Fewer scheduling events | Higher runnable-task latency; conflicts with responsiveness goal | D/E | DROP |
| `sched_min_granularity_ns` | 2 ms | No A17 override | Version-dependent | Kernel/workload-specific | Reduce context switches | Increases contention latency | D/E | DROP or kernel benchmark only |
| `sched_migration_cost_ns` | 1 ms | No A17 override | Version-dependent | SoC/cache-specific | Discourage migrations | Can block useful load balancing | E | DEVICE-SPECIFIC; benchmark |
| `sched_pelt_multiplier` | Initially 4, then 2 | No A17 override | Android/vendor extension | Kernel-specific | Faster load tracking and DVFS response | Power, oscillation, unstable governor behavior | D/E | KERNEL-SIDE; benchmark |
| `sched_schedstats` | 0 | No generic override | Config-dependent | Debug/build policy | Avoid accounting when enabled | Removes scheduler latency evidence; often dormant already | C/F | DROP runtime write |
| cpuidle governor | `teo` | Kernel selects registered/default governor | Only when TEO is built and writable | Kernel/device policy | Potentially better timer-oriented idle selection | Platform-specific idle-power and latency regression | E | DEVICE-SPECIFIC; benchmark |
| `sched_pixel/*` on policy0/4/6 | Fixed thresholds and frequencies | Not owned by system/core | Pixel kernel extension | Pixel/SoC-specific | Faster frequency response | Hard-coded topology/OPPs; power and thermal cost | E | Kernel/Power HAL only |
| Exynos MIF/INT minimums | 1014000/200000 | No generic policy | Exynos-specific | SoC-specific | Reduce memory/interconnect jank | Permanent idle-power and thermal cost | E | Power HAL/device-specific |
| Mali scheduling/power nodes | `80`, `80`, `251000 2 0` | Driver policy | Driver-specific | GPU-specific | Reduce GPU latency | Undocumented semantics and stability/power risk | E | Vendor GPU policy only |

TEO is a valid governor but is not a portable Android default. Kernel selection and bounded PM QoS requests are preferable to a global runtime override.

### Memory and reclaim

The Purgatory VM block is scoped to low-RAM devices, where several proposed values are particularly hazardous.

| Setting/change | A16 value | Current A17 behavior/default | Interface exists? | Purpose | Downside/risk | Class | Recommendation |
|---|---:|---|---|---|---|---|---|
| `dirty_background_ratio` | 10 instead of A17 5 | A17 low-RAM value 5 | Yes | Delay background writeback | Larger dirty bursts and reclaim stalls on low RAM | F | KEEP A17; DROP patch |
| `dirty_ratio` | 30 | No A17 override; upstream commonly 20 | Yes | Permit more dirty memory | Long synchronous writeback stalls and more data at risk | F | DROP |
| `dirty_expire_centisecs` | 200 | Already 200 on low-RAM | Yes | Expire after two seconds | More frequent writeback than kernel default | C | ALREADY-UPSTREAM |
| `compact_unevictable_allowed` | 0 | Usually defaults to 1 | Yes | Avoid scanning unevictable pages | Fragmentation and high-order allocation failures | E | NEEDS-BENCHMARKING |
| `compaction_proactiveness` | 0 | Kernel default commonly 20 | Modern kernels | Save background compaction CPU | Allocation-time compaction and latency spikes | E | NEEDS-BENCHMARKING |
| `oom_dump_tasks` | 0 | Kernel default generally 1 | Yes | Reduce OOM logging | Removes failure diagnosis; cost is exceptional-path only | F | DROP |
| `oom_kill_allocating_task` | 1 | Kernel default 0 | Yes | Fast victim selection | Can kill a critical allocator rather than the best victim | F | DROP |
| `stat_interval` | 1500 s | Kernel default normally 1 s | Yes | Reduce VM-stat folding | Extremely stale statistics; harms reclaim and observation | F | DROP |
| Global `swappiness` | 20 | Device/MMD/zram policy owns behavior | Yes | Reduce swapping | More app kills; ignores compressed-swap economics | E/G | DEVICE-SPECIFIC; coordinate with MMD/lmkd |
| `watermark_scale_factor` | 200 | Kernel default commonly 10 | Yes | Keep more free memory | Wastes RAM and triggers reclaim/lmkd earlier | E/F | DROP |
| MGLRU `enabled=Y` | Enabled at runtime | Kernel Kconfig/device policy | With `CONFIG_LRU_GEN` | Better aging/reclaim | Conflicts with rollout and unsupported kernels | C/G | Prefer kernel config |
| MGLRU `min_ttl_ms` | 5000 | Default 0 | With MGLRU | Protect working set | Deliberately causes earlier OOM when protected set cannot fit | F | DROP |
| PSI/lmkd | Not used by patches | PSI exposed; modern lmkd uses pressure and thrashing | Yes on supported kernels | Pressure-aware process selection | Requires device calibration | C/G | Use instead of OOM sysctls |
| Per-memcg reclaim | Not used by patches | Already implemented through `memory.reclaim` | cgroup v2 | Target reclaim at process teardown | Synchronous reclaim can add teardown latency | A/G | Controlled rollout; benchmark |

MGLRU should be enabled through kernel configuration where supported, initially with `min_ttl_ms=0`. Any TTL experiment should be small, pressure-tested, and treated as device-specific. A fixed 5000 ms value creates substantial premature-OOM risk.

### Networking

| Setting/change | A16 value | Current A17 behavior/default | Interface exists? | Benefit sought | Downside/risk | Class | Recommendation |
|---|---:|---|---|---|---|---|---|
| `net.unix.max_dgram_qlen` | 512 | A17 explicitly uses 2400 | Yes | Reduce queued datagrams | Regression under Android local-socket bursts | C/F | DROP |
| `netdev_max_backlog` | 16384 | Kernel/device default | Yes | Absorb RX bursts | Memory/latency growth; may not affect driver path | E | DEVICE-SPECIFIC; benchmark |
| TCP congestion control | `cubic` | Kernel/config default | If CUBIC built | Predictable WAN behavior | Overrides potentially better product default | C/E | KEEP kernel/netd default |
| `tcp_ecn` | 1 | Kernel/carrier policy varies | Yes | ECN negotiation | Middlebox/carrier compatibility | E | Device/network benchmark only |
| `tcp_fastopen` | 3 | Kernel/config dependent | If built | Reduce repeated handshake latency | Unneeded server side; compatibility/security surface | E | DROP global value |
| `tcp_slow_start_after_idle` | 0 | Kernel default normally 1 | Yes | Preserve congestion window | Bursts after path conditions change | E | DROP |

Android BPF/netd architecture does not remove these sysctls, but it makes blanket global tuning less defensible. Radio, carrier, RTT, loss, queueing, and power must be measured.

### Diagnostics, reliability, security, and functional behavior

| Setting/change | A16 action | Current A17 behavior | Risk assessment | Class | Recommendation |
|---|---|---|---|---|---|
| Unconditional debugfs mount | Mount at `/sys/kernel/debug` | Conditional mount/unmount; tracefs separate | Expands debug attack surface and defeats restrictions | C/F | DROP |
| `panic_on_oops` | 0 | A17 explicitly sets 1 | Continues after possible kernel corruption instead of controlled recovery | F | DROP |
| `hung_task_timeout_secs` | 0 | Already 0 | No additional benefit | C | ALREADY-UPSTREAM |
| `hung_task_check_count`, `hung_task_warnings` | 0 | Not overridden | Hides lockup evidence; timeout is already disabled generically | F | DROP |
| `soft_watchdog`, `softlockup_panic` | Disable | Kernel/device policy | Removes lockup detection/recovery | F | DROP |
| `janeiro/wdt_disable` | Disable | Vendor-specific | Disables subsystem recovery | E/F | DROP |
| EFI pstore | Disable | A17 mounts and consumes pstore | Breaks rollback, boot-reason, panic, and integrity diagnosis | F | DROP |
| devcoredump | Disable all | Retained | Dumps are failure-path, not normal hot-path work | F | DROP |
| all `sscoredump/*` | Disable | Pixel subsystem crash infrastructure | Loses AOC/GPU/WLAN/subsystem evidence | E/F | DROP or evidence-based vendor policy |
| `trace_printk` | Attempt runtime disable | Tracefs/Perfetto retained | Production callsites should be removed or replaced at build time | G | KERNEL-SIDE |
| debugfs `debug_enabled`, `sched_debug` | Disable | Vendor ABI | Optional/undocumented and depends on mounted debugfs | D/E | DROP |
| binder debug masks | 0 | No generic override | Often absent/old; compile debug overhead out instead | D/G | DROP runtime write |
| blkcg debug stats | Disable | Kernel config policy | Reasonable production concept, wrong runtime layer | G | KERNEL-SIDE |
| bcmdhd/Exynos DRM/V4L2/videobuf2/gspca/HID debug | Disable | Driver/build policy | Possible overhead only if logging is active; loses evidence | E/G | Vendor build configuration |
| EDAC corrected/uncorrected logging | Disable | Platform policy | Hides memory hardware failure, including uncorrected errors | F | DROP |
| cryptomgr tests | Disable | Kernel security policy | Disabling crypto validation is not a performance tuning | F | DROP |
| `noirqdebug` | Enable | Kernel policy | Hides stuck/spurious IRQ faults | F | DROP |
| exception trace | Disable | Arch/kernel default | Exceptional-path cost; loses evidence | F | DROP |
| SCSI logging | 0 | Normally quiet | Usually redundant; may hide storage failures | C/F | DROP |
| Wi-Fi logtrace/logdump/memdump | Disable | Vendor ABI | May save resources only if persistent logging is active; loses firmware crash evidence | E | User-build vendor policy only |
| bootreceiver tracing | Not safely addressed | LOSP already disables it by default | Existing targeted optimization retains opt-in | C/G | ALREADY-UPSTREAM |

The following are functionality changes, not performance tuning, and should be dropped: Bluetooth `disable_ertm`/`disable_esco`, RFCOMM `disable_cfc`, HID special-driver and device-behavior changes, line-discipline autoload policy, UART quirks, tunnel error suppression, hard-coded input device controls, and `fs.lease-break-time` changes.

## Explicit device-specific path ownership

None of these belongs in generic `system/core/rootdir/init.rc`:

- `17000010.devfreq_mif`, `17000020.devfreq_int`: SoC memory/interconnect policy; Power HAL, vendor init, or kernel devfreq.
- `28000000.mali`: exact GPU address and driver ABI; vendor GPU/Power HAL policy.
- `policy*/sched_pixel`: Pixel scheduler extension; Pixel kernel and Power HAL.
- `sscoredump/*`: Pixel reliability infrastructure; vendor crash policy.
- `bcmdhd4389`, `janeiro`, `dw3000`, `keydebug`, `debug_reboot`: exact vendor modules.
- `input2`: unstable enumeration; input-driver/vendor policy.
- `/sys/wifi/*`: vendor driver ABI.
- `dm-38`, `sda5`-`sda8`, `sda`-`sde`, `nullb0`: nonportable runtime names.
- fixed cpufreq policy numbers and frequencies: exact topology and OPP table.

## Concrete A17 implementation design

### 1. Generic system/core work

#### Selective `CpuPolicySpread`

- **Repository:** `system/core`; consumer repository where a measured latency-critical thread is identified.
- **Files:** `libprocessgroup/profiles/task_profiles.json`; consumer service/init file.
- **Existing support:** `UClampLatencySensitive`, `CpuPolicySpread`, and `CpuPolicyPack` already exist.
- **Change:** use `CpuPolicySpread` explicitly for selected input/render/UI-critical work. Add aggregate aliases such as `LatencySensitiveTopApp` only when a concrete consumer needs a repeated combination.
- **Do not:** add `CpuPolicySpread` to every foreground/top-app aggregate.
- **Why better:** policy follows selected work and remains abstracted from cgroup paths.
- **Risk:** low for profile definition, medium for assignment.
- **Benchmark:** required per consumer.
- **Commit:** profile definition and each consumer conversion should be separate.

#### Optional-interface handling

- **Repository:** `system/core`.
- **Files:** `libprocessgroup/task_profiles.cpp`, `task_profiles.h`, unit tests.
- **Existing support:** actions validate controller/attribute existence; `memory.reclaim` and optional reclaim syntax are probed.
- **Change:** only if field failures justify it, classify `ENOENT`, `EOPNOTSUPP`, and `EINVAL` cleanly and rate-limit unsupported optional-interface logs.
- **Do not:** add a general init `write-if-exists` solely to facilitate sysfs tuning.
- **Why better:** capability remains within the abstraction that understands the interface.
- **Risk:** low.
- **Benchmark:** not required for fallback behavior.
- **Commit:** standalone.

#### `memory.reclaim`

- **Repository:** `system/core`.
- **Files:** `libprocessgroup/flags.aconfig`, `processgroup.cpp`, `task_profiles.cpp`, tests.
- **Existing support:** full implementation and AConfig rollout flag already exist.
- **Change:** validate and roll out existing behavior; optionally skip zero-byte reclaim and add debug-only timing/reclaimed-byte metrics.
- **Why better:** targeted reclaim of a dying process rather than global swappiness/watermark/OOM changes.
- **Risk:** low-medium because synchronous reclaim can lengthen teardown.
- **Benchmark:** required before enabling rollout.
- **Commit:** instrumentation and behavior changes separate.

#### Boot prefetch

- **Repository:** `system/core` for mechanism; `device/google/laguna` for activation.
- **Files:** `init/libprefetch/prefetch/prefetch.rc`, `src/arch/android.rs`, `src/replay.rs`, `Android.bp`; Pixel init RC.
- **Existing support:** record/replay, build fingerprint invalidation, metadata storage, APEX mode, configurable I/O depth.
- **Change:** add useful-byte/page, elapsed-time, and replay budget metrics if needed. Apply an explicit low-interference task/I/O profile. Let the device choose record/replay milestones.
- **Do not:** add unconditional generic triggers.
- **Why better:** uses an observed boot working set rather than arbitrary global readahead.
- **Risk:** medium.
- **Benchmark:** mandatory.
- **Commit:** generic metrics, Pixel activation, and profile data separate.

#### Init critical path

- **Repository:** service owner or Pixel device tree; `system/core` only for proven generic dependencies.
- **Files:** `rootdir/init.rc`, imported service RC files, assembled vendor/system_ext init files.
- **Change:** trace and classify strict zygote, boot-animation, unlock, post-zygote, post-boot, and on-demand work. Move only demonstrated nondependencies. Prefer `disabled` plus activation or correct `oneshot` lifecycle.
- **Why better:** removes work from the boot critical path instead of suppressing observability.
- **Risk:** high without dependency evidence.
- **Benchmark:** mandatory.
- **Commit:** one service or tightly related group per commit.

#### Production diagnostics

- **Repository:** build owner, driver/kernel owner, or relevant service.
- **Existing support:** conditional debugfs, separate tracefs, Perfetto, pstore, debug-only packages and F2FS iostat, disabled bootreceiver instance.
- **Change:** only remove demonstrated always-running debug work from user builds. Prefer `PRODUCT_PACKAGES_DEBUG`, dormant tracepoints, rate limits, and bounded storage.
- **Do not:** disable pstore, watchdogs, dumps, tracefs, Perfetto, panic recovery, or crash collection.
- **Risk:** subsystem-dependent.
- **Benchmark:** proof of real runtime overhead required.
- **Commit:** one diagnostic subsystem per commit.

#### Dynamic storage targeting

- **Repository:** `system/core` already supplies aliases; device fstab/helper owns policy.
- **Existing support:** by-name aliases and Pixel `sysfs_path=/dev/sys/block/bootdevice`.
- **Change:** no generic change. A device helper may inspect active scheduler, physical device identity, rotational flag, and node availability.
- **Risk:** low for discovery; medium for applied values.
- **Benchmark:** values require benchmarking.
- **Commit:** helper infrastructure separate from values.

### 2. Pixel 10 device-specific work

Current Pixel 10 devices `frankel`, `blazer`, `mustang`, and `rango` map to the `muzel` family. The open `device/google/muzel` tree inherits common platform configuration from `device/google/laguna`; the latter inherits Pixel libperfmgr, Pixel memory management, thermal, storage health, Pixelstats, and interrupt rebalancing.

Existing ownership points:

| Policy | Existing hook |
|---|---|
| Power hints and ADPF | `/vendor/etc/powerhint.json` and Pixel libperfmgr |
| Power HAL implementation | `hardware/google/pixel/power-libperfmgr/` |
| MMD/zram | `device/google/laguna/product.prop`, `hardware/google/pixel/mm/device_gki.mk` |
| lmkd | Pixel memory configuration in `device_gki.mk` |
| F2FS | `device/google/laguna/conf/f2fs/fstab.rw.laguna.f2fs` |
| 16 KiB F2FS | `device/google/laguna/conf/fs-16kb/fstab.rw.laguna.16kb` |
| Storage health/metrics | storage health HAL and Pixelstats |
| Thermal | Pixel thermal HAL/configuration |
| IRQ placement | Pixel `rebalance_interrupts` |
| SKU differences | `device/google/muzel/<device>/` properties/overlays |
| Kernel modules/prebuilts | `device/google/muzel-kernels/6.6` |

#### CPU masks and vendor task profiles

- **Repository:** `device/google/laguna`; SKU override under `device/google/muzel` only if required.
- **Files:** proposed `conf/task_profiles.json`, `common.mk`.
- **Mechanism:** ship a minimal `/vendor/etc/task_profiles.json` override rather than copying the system file.
- **Prerequisite:** inspect the assembled image for an existing vendor task-profile file and verify topology through kernel capacity/frequency/topology data.
- **Change:** measured CPU masks and selective profile composition only.
- **Risk:** medium.
- **Benchmark:** mandatory.
- **Commit:** standalone.

#### `sched_pixel`

- **Repository:** Pixel kernel and `device/google/muzel` Power HAL configuration.
- **Files:** scheduler/cpufreq Pixel implementation and source-controlled `powerhint.json`.
- **Mechanism:** stable defaults in kernel; temporary policies through libperfmgr hints. Avoid fixed policy indices where capacity/policy discovery is possible.
- **Risk:** high.
- **Benchmark:** mandatory.
- **Commit:** kernel default and Power HAL behavior separate.

#### F2FS

- **Repository:** `device/google/laguna`.
- **Files:** 4 KiB and 16 KiB fstab files; optional device storage helper.
- **Existing configuration:** `atgc`, `checkpoint_merge`, `compress_cache`, fscompress, readahead 128 KiB, stable sysfs path, zoned/expansion-device support.
- **Change:** benchmark only targeted alternatives for GC, checkpoint, compression, discard, and readahead. Do not force 5 ms urgent-GC sleep.
- **Risk:** medium-high.
- **Benchmark:** mandatory, including write amplification and data safety.
- **Commit:** one storage policy per commit.

#### MQ-deadline

- **Repository:** `device/google/laguna`.
- **Files:** proposed `conf/init.storage-performance.rc`, small native helper and `Android.bp`, `common.mk`, narrowly scoped SELinux policy.
- **Mechanism:** resolve `/dev/sys/block/bootdevice`, verify the intended UFS physical queue, parse the active scheduler, check attributes, apply a single measured profile, and exit successfully when unsupported.
- **Risk:** medium.
- **Benchmark:** mandatory.
- **Commit:** helper infrastructure before parameter values.

#### PM QoS and Power HAL boosts

- **Repository:** `hardware/google/pixel` and source-controlled Pixel 10 Power HAL configuration.
- **Files:** `power-libperfmgr/aidl/`, libperfmgr JSON.
- **Existing support:** `INTERACTION`, duration bounds, ADPF sessions, dynamic uclamp voters, uclamp ceiling/floor ranges, GPU capacity, and thermal integration.
- **Change:** first import the current stock `powerhint.json` without semantic change. Then tune existing `INTERACTION` and `LAUNCH` actions. Use time-bounded PM QoS only for touch, launch, camera, display, or low-latency audio states.
- **Do not:** apply persistent per-CPU QoS or fixed frequency floors.
- **Risk:** medium-high.
- **Benchmark:** mandatory.
- **Commit:** configuration import, interaction, launch, and PM QoS each separate.

#### lmkd, zram, MMD, and MGLRU

- **Repositories:** `device/google/laguna`, `hardware/google/pixel`, Pixel kernel.
- **Files:** `product.prop`, `mm/device_gki.mk`, kernel config fragments.
- **Existing configuration:** MMD enabled, zram at 50%, `lz77eh`, 1 GiB writeback device, idle writeback, and Pixel lmkd thresholds/metrics.
- **Change:** keep MGLRU enabled by kernel config with TTL 0 baseline. Measure PSI, refaults, compression, swap/writeback, lmkd kills, and relaunches before changing MMD or lmkd. Use SKU/RAM-tier properties only when data shows a difference.
- **Risk:** high.
- **Benchmark:** mandatory and long-duration.
- **Commit:** MGLRU config, MMD/zram, and lmkd changes separate.

#### Wi-Fi and driver debug policy

- **Repository:** `device/google/muzel`, extraction configuration, driver/kernel source.
- **Mechanism:** use debug-only packaging for tools; preserve dormant crash dumps; disable or rate-limit only measured persistent logging; retain firmware dumps with quotas/privacy controls.
- **Do not:** write `/sys/wifi/*` from generic init.
- **Risk:** medium.
- **Benchmark:** required for persistent logging changes; reliability validation always required.
- **Commit:** per driver/subsystem.

#### SoC devfreq

- **Repository:** Pixel Power HAL configuration and kernel devfreq/interconnect drivers.
- **Mechanism:** confirm existing GPU/display/CPU bandwidth votes and libperfmgr nodes. Add bounded interaction/launch votes only where missed votes correlate with jank.
- **Do not:** force permanent MIF/INT minimums.
- **Risk:** high.
- **Benchmark:** mandatory.
- **Commit:** separate from CPU policy.

### 3. Kernel-side opportunities

| Workstream | Mechanism | Risk | Action |
|---|---|---:|---|
| MGLRU baseline | `CONFIG_LRU_GEN=y`, `CONFIG_LRU_GEN_ENABLED=y`, TTL 0 | Low-medium | Confirm/adopt in kernel config |
| Scheduler ABI | Verify current `sched_pixel`, latency-sensitive, and PELT extensions | Medium | Document supported ABI; remove old assumptions |
| Frequency invariance | Validate capacity/frequency reporting at all OPPs | High | Fix demonstrated defects only |
| EAS/schedutil | Validate energy model, thermal pressure, and rate limits | High | Kernel/device benchmark |
| Debug overhead | Remove active production `trace_printk`; use dormant tracepoints | Medium | Per-driver build changes |
| cpuidle | Retain kernel selection; compare TEO as full power/latency experiment | High | Benchmark only |
| I/O scheduler | Establish UFS scheduler baseline and supported controls | Medium | Kernel/device policy |
| PSI/memcg | Ensure PSI, cgroup-v2 memory, `memory.reclaim`, and zram writeback | Low-medium | Required platform features |
| Reliability | Preserve pstore, ramoops, watchdogs, panic and boot-reason data | Low | KEEP |
| Driver logging | Rate-limit hot paths; compile debug-only code out of user builds | Low-medium | Per-driver commits |

## Commit plans

### A. Minimal safe system/core plan

No immediate runtime-tuning commit is justified. If a first generic commit is required:

#### A1 — `libprocessgroup: harden optional memory.reclaim capability handling`

- **Files:** `libprocessgroup/task_profiles.cpp` and tests.
- **Summary:** preserve existence checks; classify unsupported reclaim syntax; avoid noisy errors for optional unsupported interfaces; optionally skip zero-byte reclaim.
- **Dependencies:** cgroup-v2 test fixtures.
- **Risk:** low.
- **Benchmark:** no for fallback handling; measure teardown if zero-skip or behavior changes.
- **Validation:** unit tests; boot with and without memory controller/reclaim; process-group deletion; flag-disabled behavior.
- **Rollback criteria:** cleanup regression, increased teardown latency, or loss of actionable errors.

Do not combine task profiles, prefetch, scheduler policy, or sysctls into this commit.

### B. Pixel/device-specific performance plan

#### B1 — `muzel: import source-controlled Pixel 10 powerhint configuration`

- **Repository/files:** `device/google/muzel`; source powerhint JSON, product makefiles, extraction lists/fixups.
- **Summary:** reproduce current stock configuration exactly before tuning.
- **Dependencies:** libperfmgr schema/verifier.
- **Validation:** JSON verifier, HAL startup, supported hints/modes, no behavioral delta.
- **Rollback:** rejected config or any stock behavior mismatch.

#### B2 — `muzel: tune interaction and launch hints for measured frame latency`

- **Files:** source-controlled Power HAL JSON.
- **Summary:** tune existing actions/durations; prefer uclamp and interconnect voting over permanent floors.
- **Dependencies:** B1 and benchmark baseline.
- **Validation:** jank, input latency, launch latency, power, and thermal matrix.
- **Rollback:** insignificant benefit or material energy/thermal regression.

#### B3 — `laguna: add Pixel 10 vendor task profile overrides`

- **Files:** `conf/task_profiles.json`, `common.mk`.
- **Summary:** add measured topology-specific overrides and selective latency-sensitive policy.
- **Dependencies:** assembled-image profile audit and verified topology.
- **Validation:** task-profile dump, placement traces, CTS, sustained thermal workload.
- **Rollback:** increased migrations/power or placement regression.

#### B4 — `laguna: add scheduler-aware UFS queue configuration`

- **Files:** helper source, `Android.bp`, init RC, `common.mk`, minimal SELinux policy.
- **Summary:** inspect the fstab boot-device alias and apply values only when mq-deadline and nodes are present.
- **Dependencies:** storage benchmark selecting values.
- **Validation:** all SKUs/capacities, scheduler detection, I/O latency, power, integrity stress.
- **Rollback:** node errors, worse tail latency/throughput/power, or reliability issue.

#### B5 — `laguna: calibrate MMD and lmkd for Pixel 10 memory tiers`

- **Files:** `product.prop`, optional SKU properties, Pixel memory config if broadly reusable.
- **Summary:** coordinate zram size/writeback and lmkd thresholds.
- **Dependencies:** stable MGLRU baseline and multi-day data.
- **Validation:** PSI, refaults, relaunches, lmkd kills, compression, writeback, endurance estimate.
- **Rollback:** more visible kills, thrashing, storage writes, or power.

#### B6 — `laguna: enable profile-guided boot prefetch experiment`

- **Files:** Pixel init RC and product inclusion/properties.
- **Summary:** property-gated record/replay at measured device milestones.
- **Dependencies:** prefetch metrics, budget support, reproducible profile generation.
- **Validation:** at least 30 cold boots per variant, OTA invalidation, corrupt/missing-pack recovery.
- **Rollback:** median/p95 boot regression, extra I/O, or memory-pressure regression.

### C. Kernel-side plan

#### K1 — `ANDROID: gki_defconfig: enable Multi-Gen LRU by default`

- **Files:** applicable Pixel 10 kernel config fragment.
- **Summary:** enable MGLRU through config, retain TTL 0.
- **Dependencies:** kernel 6.6 support.
- **Validation:** boot, reclaim stress, app carousel, PSI/refault A/B.
- **Rollback:** reclaim CPU, kill, suspend, or memory-pressure regression.

#### K2 — `ANDROID: scheduler: correct Pixel capacity and frequency invariance`

- **Files:** scheduler/cpufreq driver and tests, only after a defect is demonstrated.
- **Summary:** correct capacity/frequency reporting, energy model, or thermal pressure behavior.
- **Dependencies:** OPP tables and scheduler traces.
- **Validation:** frequency, placement, capacity, thermal, sustained workload.
- **Rollback:** energy oscillation, placement, or sustained-performance regression.

#### K3 — `ANDROID: debug: remove production trace_printk callsites`

- **Files:** actual affected drivers.
- **Summary:** replace hot-path `trace_printk` with disabled-by-default tracepoints where evidence remains necessary.
- **Dependencies:** identification of active callsites.
- **Validation:** build scan, trace availability, crash diagnosis, runtime overhead.
- **Rollback:** loss of required field evidence.

## Benchmark matrix

| Area | Workload | Primary metrics | Guardrail |
|---|---|---|---|
| Interaction | Scroll, shade, launcher, app switch | Frame-time p50/p90/p95/p99, missed frames, input-to-present | Energy and skin temperature |
| Launch | Cold/warm launches across app classes | First/full display, CPU time, I/O bytes | Post-launch power |
| Scheduler | Mixed foreground/background load | Wakeup latency, migrations, placement, runqueue delay | Sustained performance and energy |
| Power hints | Touch, fling, camera, game, video | Hint duration, uclamp, CPU/GPU/devfreq, jank | Expiry and return to idle |
| Memory | App carousel, camera+browser, game, pressure stress | PSI some/full, refaults, kills, relaunches, reclaim CPU | No critical-process kills |
| Zram/MMD | 24-72 hour mixed workload | Compression, swap-in/out, writeback, latency | Endurance and power |
| MGLRU | Same workload with A/B kernels | PSI, refaults, scans, reclaim CPU, kills | TTL initially 0 |
| Storage | Mixed random/sequential I/O, install/update, SQLite/fsync | p50/p95/p99 latency, throughput, CPU, queue depth | Integrity, wear, power |
| F2FS | Low free space, GC, install/update, camera recording | GC time, frame stalls, amplification | Checkpoint/data safety |
| Boot | Cold, encrypted, post-OTA, rollback | Boot milestones, critical CPU/I/O, animation jank | No dependency races |
| cpuidle/PM QoS | Screen-on idle, touch, audio, suspend | Exit latency, residency, wakeups | Standby drain |
| Network | Wi-Fi/cellular across RTT/loss | Handshake, throughput, loss, energy | Carrier/middlebox compatibility |
| Diagnostics | Panic, subsystem crash, watchdog, OOM | Artifacts, boot reason, successful recovery | No evidence loss |

Use randomized A/B builds, controlled battery and temperature, separate cold/warm-cache results, at least 30 iterations for short-run latency tests, confidence intervals, and multi-day memory/power testing.

## Final groups

### Group 1 — Safe generic A17 candidates

- Preserve current defaults and reliability infrastructure.
- Use existing task-profile/uclamp primitives selectively.
- Harden optional `memory.reclaim` handling if field evidence warrants it.
- Roll out existing targeted memcg reclaim only after measurement.
- Keep dynamic by-name storage aliases.
- Add prefetch metrics/budgets only as generic infrastructure, not automatic policy.

### Group 2 — Device-specific candidates

- CPU masks and vendor task-profile overrides.
- `sched_pixel` policy.
- Pixel Power HAL interaction/launch/PM QoS hints.
- MQ-deadline values through scheduler-aware discovery.
- F2FS GC/checkpoint/readahead policy.
- MMD/zram/lmkd calibration.
- cpuidle experiments.
- network and driver logging policy.
- GPU, MIF/INT, and other interconnect/devfreq voting.

### Group 3 — Drop

- Hard-coded CPUs, block names, partitions, policy indices, platform addresses, and input enumeration.
- Device deletion.
- `oom_kill_allocating_task=1`.
- MGLRU TTL 5000 ms.
- watermark scale 200 and VM stat interval 1500.
- larger low-RAM dirty limits.
- forced global TCP values without evidence.
- unconditional debugfs.
- disabling pstore, watchdogs, lockup diagnosis, crash dumps, EDAC, or panic recovery.
- Bluetooth/HID/RFCOMM behavior changes disguised as tuning.
- obsolete or optional scheduler/vendor writes in generic init.

### Group 4 — New A17-native opportunities

- selective `CpuPolicySpread` for measured latency-critical work;
- vendor task-profile overrides;
- ADPF/libperfmgr uclamp and interconnect policy;
- PSI/lmkd/MGLRU/MMD tuning as one memory system;
- targeted `memory.reclaim`;
- kernel build-time debug optimization with tracepoint preservation;
- scheduler-aware storage discovery;
- PM QoS with bounded hint lifetime;
- profile-guided boot prefetch;
- trace-driven boot critical-path restructuring.

## Recommended roadmap

### Phase 1

- Do not apply either Purgatory patch.
- Import and validate source-controlled Pixel 10 Power HAL configuration without behavior change.
- Verify kernel MGLRU, PSI, memcg v2, `memory.reclaim`, pstore, and watchdog configuration.
- Optionally land the small libprocessgroup capability-hardening commit.
- Establish reproducible baseline telemetry and benchmark automation.

### Phase 2

- Benchmark selective `CpuPolicySpread`.
- Tune interaction/launch hints.
- Evaluate reclaim rollout.
- Test MMD/lmkd profiles.
- Evaluate MQ-deadline and F2FS changes separately.
- Prototype boot prefetch with strict budgets.

### Phase 3

- Land verified Pixel CPU masks, scheduler policy, storage policy, devfreq, and PM QoS outside generic `system/core`.
- Split common Laguna values from SKU-specific differences.
- Keep every optional sysfs interaction capability-checked.

### Phase 4

- Prototype adaptive MGLRU TTL, dynamic interconnect votes, profile-guided prefetch, and deeper boot-graph changes independently.
- Promote only changes with statistically significant user-visible benefit and acceptable power, thermal, reliability, and diagnostic cost.

The desired LOSP A17 result is therefore smaller than the Purgatory patch set in generic init, but more effective overall: policy is placed with the device and subsystem that owns it, activation is workload-aware, interfaces are verified, and performance is evaluated without sacrificing recovery or field diagnosis.
