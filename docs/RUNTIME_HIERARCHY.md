# Runtime hierarchy evaluation

Runtime transforms use a derived cache automatically. No project setting or
gameplay API is needed. Each node retains its last evaluated local transform
alongside its world matrix. An unchanged node reuses that matrix when its parent
was also reused in the current traversal.

The key compares the exact bytes of position, quaternion and scale separately.
It does not round values or treat opposite quaternions or signed zero as equal.
A changed parent causes all its descendants to recompute, even if the parent's
new matrix happens to equal its old matrix. Evaluation preserves the existing
parent-before-child order and transform arithmetic.

New matrices must be finite and have every entry within the inclusive magnitude
limit of `1e12`. The matrix and its key are published together after validation.
A failed calculation cannot leave a new matrix paired with an old key.

## State and lifetime

The cache is internal acceleration data. It is absent from authored documents,
gameplay state, checkpoints and public schemas. New and reconstructed nodes
start invalid. Physics, animation, IK, root motion, camera changes and rollback
continue to update local transforms through their existing boundaries; the
next hierarchy traversal compares those actual values.

A cache belongs to one retained output matrix and one parent binding. Internal
code that independently replaces that output or changes the parent binding
must invalidate the cache. Current runtime parent bindings are fixed for each
node's lifetime. Entity creation and removal retain the existing structural
transaction and rollback contracts.

Traversal still visits every live node and resolves parent references. This is
an optimization of matrix evaluation, with conservative descendant propagation;
it does not make hierarchy traversal parallel or constant-time.

## Inspect reuse

Use the [native profiler](PROFILER.md#command-service) to record a workload,
stop the capture and inspect its summary or exported events. Hierarchy evaluation
emits two counters per completed traversal:

| Counter | Value |
| --- | --- |
| `runtime.hierarchy.recomputed` | Nodes whose world matrices were evaluated in this pass. |
| `runtime.hierarchy.reused` | Nodes whose existing world matrices were retained in this pass. |

The two values sum to the traversal's live membership. A tick can contain
multiple traversals; each pass counts its nodes again. These are diagnostic
observations, not persistent gameplay counters. Counter summaries report
sample statistics rather than cumulative work. Recording them consumes capture
capacity and adds instrumentation cost under the profiler's existing bounds.

`runtime.hierarchy` measures inclusive elapsed wall time for each traversal.
Enclosing synchronization, animation and batch scopes may overlap it. Do not
add their durations or interpret a reciprocal as frame rate. Profiling a
headless Debug workload does not measure GPU cost,
presented frames or production-game performance.

## Native regression

Build `poima-runtime-hierarchy-test` with native simulation and tests enabled,
then run:

```sh
ctest --test-dir build/linux-game -R '^runtime_hierarchy_native$' --output-on-failure
```

The executable also accepts `--output <new-directory>` to retain complete save
operands and observed transforms. Its seven groups cover exact cache keys and
failed publication, independent affine expectations and parent propagation,
physical descendants, animation and IK, root motion, camera/stance changes, and
structural transactions. Native qualification requires all seven groups; a
build without simulation exercises only the two portable helper groups.

Analytic matrix expectations use independent quaternion-vector arithmetic.
Whole-save comparisons retain all fields and require exact equality. Selected
contact-free or settled fixtures do not promise exact cross-process futures for
arbitrary active physics contacts.

Recorded native checks and the matched full-game comparison are in the
[0.0.200 evidence](evidence/m2-runtime-hierarchy.json). The comparison uses the
same compiled game and content on the retained 0.0.199 and new 0.0.200 Linux
Debug executables. It separates profiling-off observations from four profiled
runs in balanced A/B/B/A order, retains complete saved-state comparisons, and
includes the new counters' instrumentation cost in the new executable's times.

On this fixed workload, two profiled pairs reduce mean hierarchy traversal time
from 3.315 ms to 2.534 ms and inclusive native batch time from 18.719 ms to
16.286 ms. Each new-executable capture reuses 84.0% of node evaluations across
its three hierarchy passes per tick. This small same-laptop Debug cohort is a
descriptive measurement; it does not qualify rendered performance or explain
the earlier full-game deadline failure.
