# Android 17 generic boot-prefetch implementation report

## Scope and outcome

This audit covered the complete reusable implementation in `init/libprefetch/prefetch`. The services remain disabled and there are no generic triggers, device activation policy, device-tree changes, scheduler values, CPU masks, or `rootdir/init.rc` changes.

The implementation was already structurally sound: it records page-cache insertion tracepoints, coalesces profile ranges, protects the serialized profile with format/version checks and CRC32, gates Android use behind `ro.prefetch_boot.enabled`, invalidates profiles by build fingerprint, provides separate APEX record/replay services, limits parallelism and cached file descriptors, and has host unit tests. The patch therefore concentrates on bounded replay, truthful low-cost metrics, stale-file safety, expected-condition handling, and removal of panic paths.

## Existing architecture discovered

### Activation and lifecycle

- `prefetch.rc` declares four `disabled`, `oneshot` services: normal record/replay and APEX-only record/replay. Nothing in generic system/core starts them. A device/product must opt in with `ro.prefetch_boot.enabled=true` and explicitly choose lifecycle milestones.
- Record normally runs until `prefetch_boot.record_stop=1`; a nonzero `--duration` provides a timer-based stop. The trace reader consumes the existing trace buffer, then `trace_pipe`, and an exit channel ends collection.
- First use creates a ready marker and build-fingerprint file but intentionally does not record. On a later boot with the same fingerprint and no pack, record proceeds. An OTA/rollback fingerprint mismatch removes the old pack, updates the fingerprint, and defers recording until the next boot.
- Replay checks the enable property, pack existence, fingerprint-file existence, and exact `ro.build.fingerprint` match before parsing the pack. A missing pack is now preserved as the intended Android no-op instead of being rejected during generic argument verification.
- The APEX services use an `/apex` include prefix, a separate trace instance, pack, ready marker, and fingerprint file. The same replay engine is used.

### Recording and profile format

- The memory tracer enables `mm_filemap_add_to_page_cache` and tracing for its selected tracefs instance, restoring prior trace configuration through RAII objects.
- Virtual/transient mounts and unsuitable filesystem types are excluded. Android additionally excludes `/data` through the f2fs exclusion and excludes well-known virtual/storage prefixes. Explicit include/exclude mount prefixes are supported.
- Trace lines map `(device, inode)` to a stable profile-local `FileId`. After recording, mounted filesystems are walked to resolve inode paths and collect inode, device, size, filesystem block size, and alternate hard-link paths.
- Directory records and unresolved paths are removed. Ranges are grouped by file, sorted by offset, duplicate offsets are eliminated, and adjacent/overlapping ranges are coalesced while retaining the earliest timestamp.
- CBOR profiles contain a magic UUID, major/minor version, generation time, CRC32 digest, filesystem metadata, inode/path metadata, and chronological replay records. Custom deserialization verifies magic, version, structural references, paths, and checksum.

### Replay

- The pack is deserialized once. Optional JSON configuration supplies exclusion regular expressions. (`additional_replay_files` exists in the configuration schema but is not consumed by replay.)
- A shared record index distributes work over `io_depth` worker threads. Each worker owns a fixed 1 MiB buffer. This makes outstanding userspace read memory and active reads proportional to configured I/O depth.
- An LRU cache bounds open descriptors by `max_fds`. Filesystem speculative readahead is suppressed with `POSIX_FADV_RANDOM`, and the requested ranges are populated with chunked `pread64` calls.
- Existing replay metrics were opt-in and written beside the pack as `.stat`: successful records, nominal bytes/pages, and execution time. The generic rc services already request metrics.
- Non-strict replay continues past per-file failures; `--exit-on-error=true` stops workers after the first propagated error.

## Problems found

1. Replay had no byte or elapsed-time ceiling. A valid but unsuitable profile could consume unbounded boot I/O relative to device needs.
2. Metrics counted profile lengths, not bytes actually returned by `pread64`; EOF-short reads therefore overstated useful work. They did not expose examined records, opens, skips, categorized open/read failures, budget termination, cancellation reason, or completion time.
3. `io_depth=0` silently did no work and `max_fds=0` was not rejected.
4. Invalid exclusion regexes, metric-file creation failures, worker spawn failures, and worker panics could panic the process rather than return a diagnosable error.
5. Replay opened only the first recorded path and did not validate that the opened object still matched recorded device, inode, and size. A replaced path could warm unrelated data; valid alternate hard-link paths were not tried.
6. Deserialization accepted zero-length, overflowing, and wholly out-of-file ranges if the checksum was internally consistent.
7. Android's argument precheck rejected a missing pack before the existing Android no-profile no-op could run.
8. The binary logged genuine failures but returned success, hiding failures from init/service diagnostics.
9. Record completion had no compact duration/profile-size summary.

## Changes made

- Added optional `--max-bytes` and `--max-duration-ms` replay budgets. Both default to zero (unlimited), preserving existing behavior. Generic rc passes device-configurable `ro.prefetch_boot.max_bytes` and `ro.prefetch_boot.max_duration_ms`, also defaulting to zero.
- Byte reservations are serialized immediately before each `pread64`, so concurrent workers cannot collectively submit more than the configured byte ceiling. Missing, stale, excluded, permission-denied, and otherwise unopenable records consume no byte budget. The final submitted range is truncated to the remaining budget. Time is checked before claiming each record and before every 1 MiB read chunk. A blocking storage syscall can still overrun the wall-clock deadline; this is documented below.
- Expanded metrics with requested versus actually read bytes, examined records, successfully opened files, skipped files, open failures, read failures, budget status, cancellation reason, and boottime completion timestamp. Metrics are now written on replay failure as well as success. Existing metric names are retained where meaningful.
- `prefetched_bytes` and `prefetched_pages` now derive from bytes actually returned. A record is counted successful only when its requested range completes.
- Added a low-cost record completion log containing elapsed milliseconds, resolved file count, record count, and serialized profile bytes.
- Validate nonzero I/O depth and FD cache size. Validate regex compilation and excessively large duration conversion without panicking.
- Try all recorded paths. After opening, validate device number, inode, and exact size on the open descriptor, avoiding a path-stat/open race. Changed files are skipped as stale.
- Reject malformed zero-length, integer-overflowing, or wholly beyond-EOF ranges after checksum verification. A final page/folio may legitimately cross EOF and remains accepted.
- Convert metric-file creation, worker creation, and worker panic cases into normal errors.
- Preserve missing Android profiles as clean no-ops and return a nonzero process status for genuine execution failures.

## Changes deliberately rejected

- No global enablement or trigger was added. Record/replay milestones remain device-owned.
- No task profile was forced. `ServiceCapacityLow`, `LowIoPriority`, and other existing profiles encode policy that may make an early-boot prefetch ineffective or may be wrong at a device-selected milestone. The correct assignment requires device boot benchmarks; generic code should not guess.
- No CPU scheduler class, affinity, frequency, timer-slack, block scheduler, or storage tuning was added.
- No PSI/lmkd-based cancellation was added. PSI is system-wide, lmkd thresholds are device policy, and this library has no stable generic signal defining when cache warming becomes counterproductive. Byte/time bounds and device-owned start timing are safer generic controls.
- No cache-residency effectiveness probe was added. Repeated `mincore` or equivalent always-on instrumentation would add syscalls and perturb the workload. Requested/read bytes measure work, not downstream reuse.
- No maximum-file knob was added because byte/time ceilings already bound the primary harm and another knob would duplicate policy without evidence.
- No rewrite to io_uring, kernel `readahead`, or async I/O was attempted. The existing bounded worker model is simple and measurable.
- No profile-age cutoff was added. Fingerprint identity is the reliable generic invalidation key; acceptable wall-clock age is product policy and wall-clock validity early in boot is not guaranteed.
- No automatic deletion of a corrupt profile was added. Replay rejects it safely, but deleting an explicitly supplied profile is a policy decision. A future Android-only quarantine/re-record flow could be considered.

## Files modified

- `init/libprefetch/prefetch/prefetch.rc`
- `init/libprefetch/prefetch/src/args.rs`
- `init/libprefetch/prefetch/src/args/args_argh.rs`
- `init/libprefetch/prefetch/src/error.rs`
- `init/libprefetch/prefetch/src/format.rs`
- `init/libprefetch/prefetch/src/lib.rs`
- `init/libprefetch/prefetch/src/main.rs`
- `init/libprefetch/prefetch/src/replay.rs`
- `.codex-reference/a17-boot-prefetch-implementation.md`

## Metrics added and semantics

The `.stat` file retains `prefetched_records`, `prefetched_bytes`, `prefetched_pages`, and `exec_time_ms`, and adds:

- `requested_bytes`: aggregate byte counts passed as the requested length to `pread64`, including a request that returns an error or a short read. Open-stage failures do not increment it.
- `records_examined`: records claimed or examined by workers.
- `file_opens`: successful underlying opens (not LRU cache hits; a file may be reopened after LRU eviction).
- `records_skipped`: records excluded by configured regex.
- `open_errors`: missing, stale, permission-denied, metadata, and other open-stage failures.
- `read_errors`: failed `pread64` operations.
- `budget_exhausted`: whether a configured bound stopped or truncated replay.
- `cancellation_reason`: `none`, `bytes`, or `duration`.
- `completion_time_ns`: monotonic boottime timestamp at worker completion.

`prefetched_bytes` is now actual returned bytes. These counters are lock-local updates around work already synchronized by shared state; no new tracing or per-page probe was introduced.

Record logs one completion summary with duration, files, records, and serialized bytes. Profile generation time already exists in the header and build identity remains in the adjacent fingerprint file; neither is duplicated into every replay stat.

## Budget behavior and configuration

- `--max-bytes <u64>` / `ro.prefetch_boot.max_bytes`: maximum aggregate bytes workers may submit to storage reads. Unit: bytes. Range: `0..=u64::MAX`; `0` means unlimited. Parsing validates the integer. Each at-most-1 MiB request is reserved under shared state immediately before `pread64`, which strictly enforces the aggregate limit across workers without charging open-stage failures.
- `--max-duration-ms <u64>` / `ro.prefetch_boot.max_duration_ms`: elapsed replay limit. Unit: milliseconds. Range: zero or any value representable by the platform monotonic clock; `0` means unlimited. Oversized clock deadlines are rejected.
- Existing `--io-depth <u16>` and `--max-fds <u16>` must now be `1..=65535` when parsed through the command entry point.

Unset properties preserve prior behavior. Android property substitution supplies one argument value in each fixed service command, and the integer parser rejects malformed values. The same generic limits cover normal and APEX replay because both use the same engine and represent aggregate boot-warming policy; devices can still start the disabled services independently. The limits are mechanism only: this change deliberately supplies no device-independent numeric policy.

Partial final reads are retained. Profile ranges are normally page-derived and coalesced, but `pread64` is safe for any positive byte length and touching even part of the last range can warm useful pages. Truncation uses the remaining budget exactly and preserves a strict ceiling; stopping before an oversized record would leave configured capacity unused. `requested_bytes` reports the truncated syscall request while `prefetched_bytes` reports bytes returned, and `prefetched_records` excludes an incomplete record. With multiple workers, record claims remain serialized but the final budget recipient can depend on scheduling after file opens, as replay work distribution already does.

The duration bound is cooperative. Once any worker observes expiry under the shared mutex, cancellation is latched and no worker can claim another record or reserve another read. A `pread64` already blocked in the kernel cannot be interrupted; up to `io_depth` workers may each have one syscall of at most 1 MiB outstanding when the deadline passes.

## Locking and concurrency proof

`RecordsFile` is immutable after deserialization and is shared through `Arc` without a lock. The only runtime lock is the `SharedState` mutex, which owns record indexing, the FD LRU, budget reservation/cancellation, the first worker error, and metrics. Every state critical section is bounded to cloning a record or `Arc<File>`, updating scalar counters, or updating the LRU. File open, descriptor metadata validation, `posix_fadvise`, and `pread64` happen with no state lock held. Consequently there is no nested lock acquisition and no lock-order inversion path. Worker error publication and final metric collection acquire the same mutex only after I/O has returned.

## Process exit-status conclusion

All three commands now exit nonzero when their public operation returns `Err`. Record can propagate Android eligibility/property and fingerprint-file failures; tracer setup/read failures; timeout-thread join failure; profile create/permission/serialize/write/sync failures; and fingerprint write failures. Dump can propagate profile open/deserialize/output serialization failures. These are genuine command failures rather than expected lifecycle outcomes. Expected disabled, first-use, missing-profile, and fingerprint-mismatch Android states already return `Ok(())` before command execution and therefore still exit zero.

Android init marks a terminated service as failed for diagnostics when its process exits nonzero, but these services are both `disabled` and `oneshot`: init does not automatically restart them and no dependent action is declared here. Returning failure therefore exposes real operational faults without changing activation or retry policy. The behavior is generic CLI correctness and belongs with A2's conversion of panic/setup paths into returned errors; narrowing it to replay would continue hiding record and dump failures. No focused binary exit test was added because the current host tests expose library entry points and adding process-launch scaffolding solely for the one-line mapping would be disproportionate.

## Scheduling/task-profile conclusion

Workers currently inherit the service process scheduling, CPU, cpuset, I/O priority, and timer slack. Parallel I/O is controlled by `io_depth`; no task profile is assigned in generic rc.

Existing Android profiles combine several policy dimensions and are intended for known workload phases. Assigning a background or low-capacity profile generically could prevent replay from finishing before consumers, while an elevated profile could starve boot-critical work. Device activation code should benchmark an appropriate existing profile together with its chosen replay milestone. If multiple devices converge on the same proven combination, a later generic assignment can be justified.

## Memory-pressure conclusion

Replay intentionally populates page cache and can therefore displace another working set. It allocates one 1 MiB buffer per worker plus a bounded FD cache and deserialized profile. There is no explicit pressure response today. The new byte/time ceilings bound cache-warming work and improve cancellation latency without binding system/core to lmkd tuning.

No safe generic pause signal was found: system-wide PSI cannot attribute pressure to this replay, memcg placement is product-specific, and lmkd thresholds/milestones differ by device. Devices should benchmark under cold boot, low-memory, zram/reclaim, and concurrent zygote/app-start conditions and choose start timing and budgets accordingly.

## Profile-validity and freshness conclusion

- Magic, major/minor version, structural inode/record references, nonempty paths, and CRC32 were already checked during deserialization.
- Exact build fingerprint already invalidates across OTA and rollback and deliberately delays recording until the following boot.
- This change adds range sanity checks and validates the opened descriptor against recorded device, inode, and size. Missing/replaced files degrade per-record in non-strict mode, and alternate recorded paths are attempted.
- Exact size comparison is conservative: any size change rejects the file even if a requested prefix still exists. Integrity was not weakened to salvage stale data.
- Range validation preserves earlier valid A17 output: generated records have positive page/folio lengths and offsets below the recorded EOF. A final range crossing EOF remains accepted because the recorder can describe a full final page and `pread64` legitimately returns the existing prefix. Only zero length, `offset + length` overflow, and offsets at or beyond EOF are rejected as malformed.
- A checksum-valid profile can still contain semantically poor but in-bounds data; budgets limit its impact.
- Corrupt profiles fail every replay until replaced, removed, or invalidated by a build change. Automatic quarantine remains future work.

## Failure handling

Expected no-profile and fingerprint-mismatch states return without replay. Regex exclusions are debug-level skips. Missing, replaced, permission-denied, and other per-file failures are counted and debug-logged in best-effort mode to avoid boot log spam. Read failures remain error-logged. Strict mode propagates the first file/read error. Malformed/checksum-invalid profiles, invalid options, metric-file failures, worker failures, and other setup errors return failure and now produce a nonzero process exit.

## Tests

Added focused unit coverage for:

- concurrent aggregate byte enforcement and partial-final-record behavior;
- confirmation that claiming an unopenable/skipped record consumes no byte budget;
- duration expiry before work is claimed;
- same-size stale descriptor identity rejection;
- zero-length, overflowing, and at-EOF range rejection plus crossing-EOF compatibility;
- requested-versus-returned byte metrics, expanded metric keys, and no-cancellation state.

Existing tests continue to cover format version mismatch, checksum/structure behavior, record coalescing, missing mappings/paths, replay errors, exclusions, strict/best-effort operation, and metric byte/page counts.

## Validation results

- Project-style `rustfmt` validation of modified replay/format sources: passed.
- `git diff --check`: passed.
- `cargo test`: passed, 39 library tests plus binary/doc test targets; existing deprecation/lifetime warnings remain outside A2.
- Android Soong target build: not available from this standalone `system/core` checkout (no build environment or `m` command).

## Critical self-review

- Disabled behavior is unchanged: all services remain `disabled` and property-gated.
- Defaults preserve unbounded legacy replay unless a device supplies a budget; no magic limit was introduced.
- Memory remains bounded by the pre-existing deserialized profile, FD LRU, and one 1 MiB buffer per configured worker. Metrics add only scalar counters.
- No new boot-critical blocking path was introduced. Deadline and byte checks use the existing shared-state mutex immediately before I/O; the mutex is released before entering the kernel.
- Open-descriptor metadata validation closes the stale-path race rather than adding one.
- Expected file churn is debug-level and counted, avoiding repeated error spam.
- No device/SoC assumption, root init change, or unrelated cleanup is present.

## Remaining limitations

- Duration cancellation cannot interrupt a `pread64` already blocked in the kernel; multiple workers may each have one active request of at most 1 MiB, so elapsed-time overshoot is not bounded on broken storage. The byte ceiling itself cannot overshoot because those requests were reserved first.
- There is no external cancellation property for replay and no generic memory-pressure pause.
- Metrics quantify replay work, not later cache hits or boot-time benefit.
- Error metrics group all open-stage errors; richer errno categories could be added only if field diagnosis shows value.
- Configuration schema field `additional_replay_files` remains unused.
- Profile creation is not an atomic temp-file/rename transaction. CRC detects partial writes, but corrupt profiles are not automatically quarantined.
- The profile is fully deserialized before replay; its memory footprint is not separately capped. The profile is trusted metadata generated locally and checksum-validated, but a future format-level count/size ceiling could further harden it.
- Record stop still depends on receiving a trace-pipe line after the stop signal, as documented by the existing tracer TODO.

## Benchmark requirements

Before any device enables or tunes this mechanism, measure at least:

1. cold and warm boot-to-animation, boot-complete, unlock, and first-app milestones with replay off/on;
2. `.stat` requested/read bytes, errors, completion time, budget termination, and profile size/age;
3. page-cache residency/reuse sampled outside the production path, major faults, storage throughput/latency, PSI, reclaim, zram, and lmkd activity;
4. slow versus fast storage and low-memory versus normal-memory configurations;
5. I/O depths, FD limits, candidate byte/time budgets, activation milestones, and any proposed existing task profile;
6. OTA first boot, deferred record boot, rollback, missing/corrupt profile, changed files, and repeated-boot stability;
7. power and thermal impact plus boot-critical service scheduling latency.

Adopt values only when end-to-end boot wins are statistically repeatable without regressions in reclaim, launch latency, power, or reliability.

## Recommended commit message

Subject:

`init: bound and instrument generic boot prefetch replay`

Body:

```
Add optional byte and elapsed-time budgets to libprefetch replay while
preserving unlimited behavior when they are unset. Enforce the byte limit
across workers and check time between bounded read chunks.

Report requested and actual bytes, examined records, file/open/read outcomes,
budget termination, cancellation reason, and completion time. Validate opened
files against recorded inode/device/size metadata, try alternate paths, reject
malformed ranges, and convert replay panic paths to errors.

Keep all generic services disabled and leave activation timing, budget values,
and task-profile policy to devices after boot benchmarking.
```

## Git status --short

The final status is reproduced in the task handoff; the report itself is an untracked file under the pre-existing untracked `.codex-reference/` directory until the user chooses what to stage.
