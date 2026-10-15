# Performance Yard reliability

The Windows reliability harness exercises the existing compiled
[Performance Yard](../examples/performance-yard/README.md) game on the native
interactive clock. The autonomous C# controller supplies movement through
native navigation. Python observes and invokes compiled menu actions; it does
not supply per-tick movement or replay.

## Run the check

Prepare the autonomous world, workload manifest and Windows Native AOT artifact
using the sample instructions. Use native Windows Python and a free desktop
window that can retain foreground focus:

```powershell
python tests/performance_yard_soak.py --binary build/windows-runtime/poima.exe --world build/performance-world/world.json --descriptor build/performance-artifact/native-gameplay.json --workload-manifest build/performance-world/manifest.json --output build/reliability --gpu 1
```

Discover the appropriate GPU index on another machine. The output directory
must be new. The test starts at 1920×1080 with two frame slots, deferred lighting,
medium GTAO, native resolution and actual native audio output. It does not build
or download dependencies. Run it under an owner that can terminate its complete
process tree if interrupted. The harness also has a one-hour deadline and exact
owned-process cleanup.

A shorter harness rehearsal exercises the same save/restore and fresh-process
logic without qualifying the reliability duration:

```powershell
python tests/performance_yard_soak.py --binary build/windows-runtime/poima.exe --world build/performance-world/world.json --descriptor build/performance-artifact/native-gameplay.json --workload-manifest build/performance-world/manifest.json --output build/reliability-rehearsal --gpu 1 --rehearsal --cycles 2 --warmup 10 --before-save-seconds 10 --after-save-seconds 5 --continuation-seconds 30 --timeout 600
```

## Optional GPU-memory recording

Add `--record-gpu-memory` to the existing soak or diagnostic-wrapper command to
retain the heap telemetry already returned by player inspection. The option is
off by default. Tooling 0.0.154 supplies the recorder; Vulkan native 0.0.152
introduced the optional report. It adds no native request, sampling thread or
driver query and preserves the original workload, foreground, clock, frame-time
and save checks.

`evidence.json.gpu_memory_observations` records first/last observation tags,
cached sample counts, per-heap observed allocation/budget ranges, separate
owner/device continuity segments and terminal reports. Session changes after
Load are annotated without treating them as a new graphics owner. Native sample
times and the recorder's elapsed observation times have separate clock domains.
Absent telemetry, explicit null, unsupported/invalid samples and measured zero
remain distinct. Older engines can still run the original checks. A cache reset
or changed device starts a separate observation segment; it does not establish
an unexposed native renderer-context identity.

The recorder retains at most 4,096 records within a 16 MiB compact-JSON budget,
including its summaries. Inspect both `complete` and `contract_ok`: omissions
and diagnostic errors are recorded separately from gameplay success. A passing
game result alone does not qualify the telemetry. `complete` means no recording
omissions; it does not imply that usable measurements exist. Check
`legacy_unavailable`, each segment’s `usable_samples`, `omission_reasons` and
`error_counts`. The full collection is snapshotted at finalization; ordinary
observations process bounded sample data.

These are sampled driver allocation estimates, not residency or lifetime peaks.
Separate contexts are never summed, and these values do not pass the Alpha
VRAM ceiling or establish a memory leak's cause. Recording has CPU/storage
cost; its overhead on representative play has not been measured. Portable
checks and offline ingestion of retained native reports qualify the recorder,
not a new foreground, frame-rate or reliability run.
[Results and limits](evidence/m2-gpu-memory-observations.json).

## Optional process-memory timeline

Add `--record-memory-timeline` to record each actual Windows memory-query
interval alongside bounded harness phase marks in one shared integer clock.
The option is off by default and also works through the diagnostic wrapper.
It adds no native request or sampling thread and preserves the existing
sampling interval and gameplay, focus, save, clock and cleanup checks.

The final `evidence.json.memory_timeline` links each owner RAM report to its
conservative phase associations. Queries crossing boundaries remain
unassigned. A requested recording error preserves the original native error
and cleanup, but fails the recording and overall result. The phase trace is
bounded to 4,096 marks/1 MiB, with a 16 MiB combined timeline/RAM-report budget.
These observations do not diagnose allocation causes or qualify growth,
residency or representative recording overhead.
[Clock, bounds, recovery and verification](MEMORY_TIMELINE.md).

## What it checks

The full run warms up for 60 seconds, then performs 40 Save/Load cycles. Each
cycle includes 35 seconds of real play before Save and 10 seconds afterward
whose progress Load must discard. At least 1,800 cumulative seconds of unpaused
play are required. Menu actions, checkpoint inspection, captures and planned pauses
are outside that credited interval. This is not one uninterrupted 30-minute
interval.

Each checkpoint observes all authored and spawned entities, supported native
poses, physics and animation state, registered components, gameplay fields, UI
and retained sound voices. Load and its immediate retry must restore the same
observed state; retrying Save must leave it unchanged. Only opaque runtime
session identity is normalized. Store generations, payload hashes, immutable
content and native gameplay artifact inventories are checked separately.

The first cycle changes FOV from 60 to 65, UI scale from 1 to 1.25 and master
gain from 1 to 0.9 through the compiled menu. Those current preferences must
survive subsequent Loads without entering the gameplay save. A fresh engine
process restores the last checkpoint with its own default preferences and
continues navigation for another 60 seconds. This loads the same gameplay
image; it does not qualify in-process Native AOT code reload.

Foreground focus, clock progress, geometry and native audio submission are
checked at bounded heartbeats. Twelve short frame-recorder windows sample
timing and GPU retirement; paging and compression happen while paused. Process
working set and private commit are sampled separately through observed play and
checkpoints, including planned pauses. Sampling starts after player readiness
and ends before final stop/drain and host shutdown. Raw RPC logs and complete local checkpoint
observations remain in the selected output for diagnosis.

## Scope

This is a warm, bounded workload with simple geometry and imported Idle rigs.
Exactness covers supported reflected state, rather than every internal physics
or renderer cache. Timing windows and focus observations do not cover every
frame of the entire run. Native audio progress and drain do not qualify physical
listening or acoustic quality. Process-memory samples do not measure GPU VRAM
or prove the absence of leaks.

Compatible CoreCLR reload, scene edits, physical controls, production FPS
complexity and clean-install/export qualification have separate acceptance
cases. The complete [alpha reliability gate](ALPHA_ROADMAP.md#qualification-target)
remains broader than this fixed-image test.

## First longer attempt

The retained 0.0.96 Windows engine completed 23 cycles and 1,035.611 cumulative
seconds of unpaused play. All 92 complete checkpoint observations matched,
including immediate Save and Load retries. The run then failed its clock guard
in cycle 24: the native interactive clock discarded 0.759537 seconds, reaching
0.802427 seconds during cleanup. This is a failed reliability result. The
1,800-second requirement and fresh-process continuation were not reached.

The first failing inspection also reports a viewport change from 1920×1080 to
1920×1051. Its cause and relationship to the stall are unknown. Seven earlier
short frame windows passed their timing budgets; none covers the failing
interval. Existing audio queue-wait measurements do not include all device
operations and cannot establish the stall's cause.

The native host and client exited normally, with successful final rendering,
no validation errors and drained WASAPI audio. The memory sampler retained
1,405 observations, with a maximum working set of 400,003,072 bytes and private
commit of 636,809,216 bytes. These observations do not prove that the engine is
free of leaks. Two earlier short rehearsals passed their narrower harness
checks. [Failed attempt and raw observations](evidence/m2-performance-yard-reliability-failure.json).

## Bounded diagnostic run — 0.0.98

The [recorded result](evidence/m2-performance-yard-reliability.json) uses the
retained 0.0.97 Windows engine and 0.0.94 Native AOT gameplay image on one laptop's
RTX 4070 Laptop GPU. It passed 40 same-window checkpoint replacements over
1,801.04675 cumulative seconds of unpaused play, followed by 60.00852 seconds of
fresh-process continuation. All 161 complete state observations matched;
40 immediate Save retries and 40 immediate Load retries preserved their
receipts and committed state. Four native/client owners exited cleanly.

Use the diagnostic wrapper with the same prepared inputs and process-tree owner:

```powershell
python tests/performance_yard_reliability_diagnostics.py --binary build/windows-runtime/poima.exe --world build/performance-world/world.json --descriptor build/performance-artifact/native-gameplay.json --workload-manifest build/performance-world/manifest.json --output build/reliability-diagnostics --gpu 1
```

The wrapper preserves the original clock policy, deadline and 512 MiB RPC cap.
It retains selected paused post-restore and terminal diagnostic rings, including
their preceding poll, without advancing simulation. This run retained 41 reads
with no omissions, invalid samples or discarded clock time. Optional reads can
be omitted explicitly to reserve failure and terminal space. Its bounded setup
foreground recovery sends no input or unlock commands; that fallback was not
exercised in the passing run.

Twelve roughly ten-second recorder windows passed their sampled timing budgets;
the worst eligible present-return p95 was 13.3114 ms. These windows do not cover
every frame, measure scanout/input latency or qualify other GPUs. Working set
and private commit passed the workload's 4 GB screens; VRAM and leak freedom
remain unqualified. [Compressed observations](evidence/performance-yard-reliability-native.json.gz)
retain the bounded numeric evidence.

An earlier diagnostic rehearsal failed foreground setup before measured play;
a second passed its shorter checks. Both remain recorded with this cohort.
The earlier longer attempt above remains a failure with unknown cause; this
later passing run does not establish a causal fix or diagnostic overhead.
Compatible development reload and authored changes have a separate
[Relay Yard iteration check](RELAY_YARD_ITERATION.md). Physical interaction,
listening, clean-install delivery, production complexity and the full Alpha
gate remain outside this fixed-image result.
