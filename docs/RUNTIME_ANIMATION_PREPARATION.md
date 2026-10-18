# Animation preparation at batch boundaries

From 0.0.191, a runtime batch prepares caller animation controls only when at
least one caller animation, IK or root-motion list is nonempty. A batch with
three empty lists keeps the last committed pose until normal tick evaluation.
It still samples animation on every committed tick.

The change does not cache poses across ticks or remove gameplay-command,
pre-gameplay IK, structural-change, restore or rollback evaluation. Explicit
caller commands retain their preparation before the first tick. Physics,
input, root-motion interval commits and final synchronization keep their order.
The native ABI, command/result shapes and save formats are unchanged.

## Matched workload

The 0.0.191 comparison uses sealed Windows 0.0.190 and 0.0.191 executables,
the same qualified 0.0.185 CoreCLR bridge and the original Performance Yard
gameplay and assets. The original game and bridge SDK hashes differ; their
actual combination is retained rather than presented as identical SDKs.

The world contains 2,394 authored entities, 24 animated rigs, 1,464 rig nodes
and 64 lights. Gameplay creates three additional live entities. All fifty
globals, four component schemas and fourteen saved payload members remain.
The measured windows contain no caller animation/IK/root commands or configured
IK; these predicates are checked against the original inputs and observations.

Six fresh owners run in order 190, 191, 191, 190, 190, 191. Each warms up for
120 ticks, then records three 120-tick windows. Every tick uses the same guard
inspection followed by a one-tick step. Both versions configure two native job
workers and set `DOTNET_PROCESSOR_COUNT=2`; no CPU affinity is imposed. The
laptop is a Ryzen 7 8845HS with 16 GB RAM; the
pre-run power query reports AC power and 100% battery. No compiler, renderer or
explicit collection runs during the comparison.

## Observed work and timings

Each version contributes 1,080 measured ticks across three owners. The complete
run contains 2,880 ticks including warmup, 6,048 RPC pairs and eighteen profiler
captures containing 129,600 events.

| Measured scope count per version | 0.0.190 | 0.0.191 |
| --- | ---: | ---: |
| Animation sampling | 2,160 | 1,080 |
| Rig sampling jobs | 51,840 | 25,920 |
| Synchronization / hierarchy, each | 4,320 | 3,240 |
| Batch / tick / checkpoint, each | 1,080 | 1,080 |

Every individual 120-tick window matches its prospective count prediction.
These counts demonstrate the removed preparation work; timing changes also
include cache, scheduling and ordinary runtime effects.

The following values are inclusive elapsed milliseconds per committed measured
tick, pooled over all three owners of each version:

| Scope | 0.0.190 | 0.0.191 |
| --- | ---: | ---: |
| Native batch | 2.400 | 1.880 |
| Native step request | 2.487 | 1.962 |
| Animation sampling | 0.807 | 0.422 |
| Synchronization | 1.315 | 1.193 |
| Inner runtime tick | 0.941 | 1.466 |
| External guarded RPC loop | 11.931 | 11.293 |

Native batch and external helper means decrease in all three pairs. The inner
`runtime.tick` mean **increases**. This is not a universal tick-speed improvement
or a causal decomposition. Nested regions overlap; worker spans can overlap
each other. Do not add these rows or interpret them as presented frame times.
The helper includes transport, Python validation and retained trace writes.

## State and qualification

The original twenty-four complete v7 checkpoint references and twelve full-state
epoch forecasts agree with their observed outcomes. Fresh owners have different
random save epochs, so original cross-owner payloads are not byte-identical.
Before each counterpart's first tick, a complete forecast accounts for the
already observed destination epoch; every other typed and lexical field stays
included. No observed checkpoint is rewritten and no field is excluded.

All six native owners exit zero. Their enclosing seven-process Windows Job is
empty with zero terminated processes at closure and no forced cleanup.
Retained-file validators reconstruct all selected RPC, state and profiler data;
their source was separately reviewed. These are validation results from the
implementation's authors, not an external benchmark certification.

Seven separate Windows native executables check runtime physics, animation,
layers, IK, root motion, membership and saves. The root-motion executable runs
eleven native groups, including explicit caller-list boundaries, same-tick state,
partitioned continuation, restoration and late-failure rollback. Core and version
checks also pass. Linux's simulation-disabled build separately runs seven
portable root-motion groups and authoring/version checks; it does not execute
the new simulation-only integration group.

[Selected results and artifact identities](evidence/m2-animation-preparation.json)
retain pooled and per-owner statistics, adverse observations and scope limits.
Exact reproduction needs the identified world, compiled gameplay, bridge, SDK
closures and schedule. A new build or a different batch cadence is another
measurement. The supported [fixed-step runtime](RUNTIME.md) and
[profiler](PROFILER.md) provide the public operations for a new investigation.
This short CPU workload does not qualify rendered FPS, input latency, natural GC
timing, memory-growth causes, thirty-minute reliability or the Alpha release.
