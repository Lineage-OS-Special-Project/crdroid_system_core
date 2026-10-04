# Android 16 boot-prefetch A2 backport

## A16/A17 structural differences

- A16 has the same `prefetch` binary/library split and the same `prefetch.rc`, `args`, `format`, `lib`, `main`, and synchronous replay-worker organization as the A17 reference base.
- Both versions use the same checksum-protected CBOR `RecordsFile` profile format (major 0, minor 1), the same inode/path metadata, and the same record fields. No format-version change was required.
- Record, replay, and dump are the same argh subcommands, and replay uses the same thread-per-I/O-depth model with synchronous `pread64` calls and 1 MiB buffers.
- A16 predates the upstream A17 replay-metrics prerequisite. It had no `record_metrics` argument, stat file, replay counters, page counting, or timing in `Replay`; the minimal prerequisite functionality was included with the A2 counters.
- A16 wrapped the immutable `RecordsFile` in `Arc<RwLock<_>>` and repeatedly acquired read locks. A17 A2 uses `Arc<RecordsFile>` directly.
- The monotonic clock facilities are structurally compatible: deadlines use `Instant`; completion instrumentation uses the existing `nanoseconds_since_boot()` helper, as in A17.
- Android profile gating differs in naming/signature: A16 uses `ensure_record_is_ready()` and `write_build_fingerprint(&RecordArgs)`, while the A17 reference base uses newer helpers/signatures. The A2 behavior was mapped to the A16 API rather than importing those unrelated changes.
- A16's rc lacks the newer explicit APEX fingerprint-path option present in the A17 base. It was intentionally not added because it is unrelated to A2.
- A16 tests lacked the newer replay metrics test and copied-profile inode refresh required by descriptor validation. Existing replay tests were adapted by refreshing copied file identity.

## Files changed

- `init/libprefetch/prefetch/prefetch.rc`
- `init/libprefetch/prefetch/src/args.rs`
- `init/libprefetch/prefetch/src/args/args_argh.rs`
- `init/libprefetch/prefetch/src/error.rs`
- `init/libprefetch/prefetch/src/format.rs`
- `init/libprefetch/prefetch/src/lib.rs`
- `init/libprefetch/prefetch/src/main.rs`
- `init/libprefetch/prefetch/src/replay.rs`
- `.codex-reference/a16-boot-prefetch-backport.md`

## Semantic mapping from A17 A2

- Added `--max-bytes`, `--max-duration-ms`, and `--record-metrics` to A16's existing argh replay arguments.
- Added replay argument validation for zero I/O depth and zero FD cache size, and retained Android's missing-profile decision in `can_perform_replay()`.
- Added strict synchronized read reservations, deadline cancellation, chunk-level checks, A17 replay metrics, descriptor validation, malformed-range rejection, panic-to-error conversions, record completion logging, and nonzero process exit on command errors.
- Added the two generic rc property expansions with zero/unset defaults and did not add triggers, enablement, scheduling, task-profile, memory-pressure, or device policy.

## Behavior already present in A16

- Replay already used a 1 MiB `READ_SZ` and split large ranges into bounded synchronous `pread64` calls.
- Profiles already contained device, inode, size, and alternate path metadata.
- Profiles already had structural consistency and checksum validation.
- Android replay gating already treated a missing pack or fingerprint as a clean no-op after enablement checking.
- `exit_on_error` already controlled whether individual open/read failures abort replay; A2 preserves that established policy while ensuring strict failures propagate.

## Byte-budget implementation

Zero maps to `None` (unlimited). Each worker reserves immediately before each `pread64` while holding the shared-state mutex. The same lock protects the aggregate submitted-byte counter, preventing oversubscription across workers. Failed opens reserve nothing. A final reservation is shortened to the exact remaining budget and latches byte cancellation, matching A17.

## Duration/cancellation implementation

A nonzero duration is converted to a checked `Instant` deadline. Cancellation is latched in shared state. Workers check it before claiming a record and before every chunk reservation, which also provides between-chunk checks. An in-progress blocking `pread64` is not interrupted and may overrun the deadline.

## Metrics implementation

The optional sibling `.stat` file records completed records, submitted/requested bytes, bytes actually returned by reads, pages, records examined, successful file opens, skipped records, open/read errors, budget status, cancellation reason, elapsed replay time, and completion time. Updates use the existing shared-state mutex only at work claims, FD-cache operations, reservations, and completion accounting; no lock is held across I/O.

## Stale/range validation

Every candidate path is opened first and its descriptor metadata is checked against recorded device, inode, and size. Stale or unusable primary paths fall through to recorded alternates. Deserialization rejects zero-length, overflowing, and wholly beyond-EOF ranges. A range beginning before EOF remains valid when its nominal end crosses EOF.

## Error propagation

The CLI now exits 1 when record, replay, or dump returns an error. Profile/config deserialize errors, invalid regexes, stat-file creation failures, thread-spawn failures, and worker panics are returned instead of panicking or falsely succeeding. Android's absent optional profile remains a clean no-op; corrupt profiles do not.

## Concurrency/locking findings

A16 had the same redundant nested `RecordsFile` read locking addressed by A17. It is removed because replay never mutates the profile. The existing shared-state mutex remains responsible for the LRU FD cache, record index, result, metrics, cancellation latch, and byte reservations. It is never held during open or read syscalls.

## Tests added/updated

- Parsing, unlimited defaults, and malformed values for both new limit options.
- Strict aggregate concurrent reservations, partial final reservation, no budget charge at record claim/open-failure stage, and expired-duration claim cancellation.
- Replaced-file rejection, valid alternate-path fallback, malformed range rejection, overflow rejection, beyond-EOF rejection, and final range crossing EOF acceptance.
- Existing replay tests were retained and adapted for descriptor identity validation.

## Validation performed

- `cargo test`: passed, 42 tests, 0 failures (plus one pre-existing lifetime-style warning in `src/tracer/mem.rs`). The first sandboxed attempt could not resolve crates.io; after dependency download was authorized, the suite compiled and passed twice.
- `git diff --check`: passed.
- `cargo fmt -- --check`: ran but reports extensive pre-existing formatting differences throughout the crate, including untouched files. No bulk formatting was applied because it would violate the requested scope.
- Generated Cargo `target/` output was removed with `cargo clean` after validation.

## Limitations

- Host Cargo tests exercise the generic Linux implementation, not Android init property expansion or an Android device boot.
- The existing `nanoseconds_since_boot()` implementation is retained unchanged; A2 uses it exactly as the A17 reference does for completion metrics.
- A17's newer upstream metrics test was not imported wholesale; focused A16-compatible tests cover the new reservation, cancellation, descriptor, range, and argument behavior, while the full pre-existing replay suite covers normal/error replay.

## Intentionally not ported

- A17 API renames/refactors around Android record readiness and fingerprint writing.
- The unrelated APEX fingerprint-path rc change.
- Any device defaults, automatic enablement, triggers, task profiles, scheduler/affinity/I/O priority changes, memory-pressure coupling, vendor policy, or profile-format revision.

## Recommended commit subject

`init: bound and instrument generic boot prefetch replay`

## Recommended commit body

```text
Add optional byte and elapsed-time budgets to libprefetch replay while
preserving unlimited behavior when they are unset. Enforce the byte limit
across workers at read reservation time and check duration before claiming
new work and between bounded read chunks.

Report requested and actual bytes, examined records, file/open/read outcomes,
budget termination, cancellation reason, and completion time. Validate opened
files against recorded inode/device/size metadata, try alternate paths, reject
malformed ranges, and propagate command and replay failures.

Keep all generic services disabled and leave activation timing, budget values,
and task-profile policy to devices after boot benchmarking.
```

## git status --short

```text
 M init/libprefetch/prefetch/prefetch.rc
 M init/libprefetch/prefetch/src/args.rs
 M init/libprefetch/prefetch/src/args/args_argh.rs
 M init/libprefetch/prefetch/src/error.rs
 M init/libprefetch/prefetch/src/format.rs
 M init/libprefetch/prefetch/src/lib.rs
 M init/libprefetch/prefetch/src/main.rs
 M init/libprefetch/prefetch/src/replay.rs
?? .codex-reference/
```
