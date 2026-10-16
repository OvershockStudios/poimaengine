# Performance Yard CoreCLR workflow

Performance Yard exercises compilation, live C# replacement, saves and
fresh-process continuation in a 2,394-entity scene. Qualification checkpoint
0.0.168 uses the sealed native 0.0.167 engine. The scene retains 24 animated characters, 64 lights and its
existing geometry and assets.

## Prerequisites and authoring

The recorded Windows x64 setup uses an RTX 4070 Laptop GPU, .NET SDK 10.0.401,
hostfxr 10.0.12, the
installed authoring pack's binary SDK and generator, and the matching managed
bridge with its dependency/runtime configuration files. Keep these complete
closures together. See [authoring tools](AUTHORING_TOOLS.md) and
[managed diagnostics](MANAGED_CALLBACK_METRICS.md).

Compile the four sample gameplay sources through `development.compile`.
An accepted receipt starts a job; inspect `development.inspect` and
`development.diagnostics` for its actual outcome. The recorded sequence contains
a successful baseline, intentional syntax failure and compatible body change.
The failed build preserves the previous assembly, complete game state and
external save configuration, queue and files.

In this cohort, generated component manifests and authored registries retain
different numeric token types. Here, all four raw generated schemas exactly match the
retained NativeAOT descriptor's raw schemas. A separate headless owner imports
that manifest using `component.schema.import`, then reads `component.schemas`:
the complete canonical response exactly matches the authored registry. The
verifier does not cast numbers or delete differing fields. This owner closes
before rendered game owners start.

## Load, replace and restore

Start a runtime, then call guarded `runtime.gameplay.load` with the selected
assembly, bridge and hostfxr. Use current session, tick, gameplay revision and
structure revision. Repeating the identical completed request returns its
receipt; changing a request requires a new identity. On a conflict, inspect
current state before choosing the next action. An unknown transport outcome
fails this check; its supervising process owner retains responsibility for
diagnosis and cleanup. Do not start a duplicate producer to recover it.

The compatible body replacement preserves all 50 globals and complete paused
checkpoint state. Only the assembly digest, gameplay revision and derived
checksum change. One separately declared Tick proves changed behavior through
an existing counter; that counter is not a motion-performance measurement.

The run records eight complete world-save documents with v7 snapshots and five byte-exact,
typed-exact whole-document pairs. Warm restoration and a fresh cold process
restore the same saved document before further callbacks. These are new
CoreCLR saves; the earlier NativeAOT descriptor supplies a schema reference,
not an interchangeable save lineage.

Warm callback profiling is explicitly enabled. Reload creates fresh counters;
restoration also creates fresh counters and skips Initialize. Cold trusted
configuration defaults profiling off. Counters are diagnostics outside saved
authoritative state, not heap-size measurements.

## Clock observations and limits

Two nonfocusable background owners render at 1080p, authored 50-degree vertical
FOV, medium GTAO and native resolution, with audio and physical input disabled.
Five fixed five-second play segments establish at least 25.3456659 seconds
between observation events. For a first-read call interval [a,b] and later
pause interval [c,d], elapsed time lies within [c−b,d−a]. The entire resulting
tick-rate interval must fit 58–62 ticks/s. RPC overhead is included in this
uncertainty; these are neither frame times nor matched performance results.

The memory timeline retains 41 actual OS queries across warm and cold owners.
The run preserves normal Windows USERPROFILE while isolating application-data
and temporary directories. Driver/cache state remains uncontrolled. Earlier
preparation, startup, observer-selection and timing failures remain failed;
successful startup does not diagnose their cause.

Reproducing this exact cohort requires its pinned world, tool/runtime closures,
source versions and original receipts. It is not a clean-checkout command.
Thirty-minute reliability, recording overhead, continuing memory growth,
foreground frame budgets, clean-machine delivery and the wider Alpha gates
remain separate qualifications.

[Original results and retained failures](evidence/m2-coreclr-performance-yard.json).

The later [mixed CoreCLR cohort](PERFORMANCE_YARD_MIXED_RELIABILITY.md) combines
this prerequisite with twelve longer save/load cycles, six body reloads and
sixteen live scalar/texture applies. It establishes over thirty minutes of
verified unpaused play. Its original records and memory observations are separate
from this short cohort; growth causes, observer cost and wider Alpha gates stay open.
