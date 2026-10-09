# Native job execution

Poima's native animation sampler and procedural material baker use a shared
executor for owned, independent CPU work. The authoritative registry, physics,
C# callbacks and result publication stay on their creating owner thread. Jobs
are a core service; they do not depend on the desktop editor or a hosted service.

## Worker policy

Constructing the first runtime, or submitting the first material bake, obtains
the owner thread's pool.
Subsequent consumers on that owner share it while at least one retains it.
Opening an authored world or inspecting `jobs.status` does not create worker
threads; starting its runtime does.
The default is two workers, excluding the owner. Set `POIMA_JOB_WORKERS` to one
ASCII digit from `0` through `8` before first use to choose another count.
Zero selects synchronous reference execution, including material CPU work.
An invalid value rejects pool creation. Changing the environment does not
reconfigure an existing pool.

For example, from Linux:

```sh
POIMA_JOB_WORKERS=2 ./build/runtime-headless/poima world build/game/world.json
```

From PowerShell, set `$env:POIMA_JOB_WORKERS = '2'` before launching the Windows
executable. The world file's parent directory must exist.

```json
{"jsonrpc":"2.0","id":1,"method":"world.describe","params":{"view":"method","name":"jobs.status"}}
{"jsonrpc":"2.0","id":2,"method":"jobs.status","params":{}}
```

Status reports `initialized: false` until a pool exists. An initialized result
includes actual worker/reservation counts, serial mode, executing tasks,
frame/background occupancy and limits, high-water marks and trace loss. Its
scope is the owner thread, not one world: counts can include another consumer
on the same owner. Explicitly supplied native executors are independent of
this default-pool inspection. Creating consumers on different owner threads
creates separate pools; this is not a process-wide CPU-budget coordinator.

## Scheduling and ownership

Frame and background admission have separate bounded budgets. The default
frame lane permits eight active groups, 2,048 tasks and 8,192 dependency links;
background permits eight groups, 64 tasks and 256 links. A group supplies its
complete dependency DAG before any callback is admitted. Invalid graphs or
insufficient capacity reject without executing a partial submission.

With two or more workers, one is reserved for frame tasks and never runs
background callbacks. One worker prioritizes queued frame work, but a running
native background callback is not preempted. Owner waits can help only the
specified frame group; they never run another group's or background callbacks.

Submission, wait, trace collection and shutdown belong to the creating owner.
Callbacks cannot submit, wait for, acquire or destroy executors. They may poll
or request cancellation. Keep every borrowed input/output alive until the group
is terminal. Terminal publication also waits for executed, cancelled and skipped
callback captures to be destroyed; admission capacity is held through that cleanup.
Retained failure objects may outlive the group through result handles.
Native callbacks cooperate with cancellation; shutdown fences callback cleanup
and cannot forcibly terminate arbitrary native code. Scheduler storage is
bounded; arbitrary callback captures and outputs remain the caller's budget.

Use [`poima/jobs.hpp`](../include/poima/jobs.hpp) for the compiled native API.
A directly constructed native `Config` defaults to zero workers; the shared
owner factory applies the two-worker/environment policy described above.
C# callbacks currently run serially on the runtime owner; this does not expose
arbitrary parallel C# access to entities or components.

## Native example

This complete program runs two independent tasks and a dependent sum. Declare
borrowed storage before the executor so it also outlives shutdown on an exception
path. Each independent task writes a different array element; the dependent task
runs only after both succeed.

```cpp
#include "poima/jobs.hpp"
#include <array>
#include <iostream>
#include <utility>
#include <vector>

int main() {
    std::array<int, 2> values{};
    int total = 0;
    poima::jobs::Config config;
    config.workers = 2;
    poima::jobs::Executor executor(config);
    std::vector<poima::jobs::Task> tasks;
    tasks.push_back({[&](poima::jobs::Context&) { values[0] = 40; }, {}, "example.left"});
    tasks.push_back({[&](poima::jobs::Context&) { values[1] = 2; }, {}, "example.right"});
    tasks.push_back({[&](poima::jobs::Context&) { total = values[0] + values[1]; },
                     {0, 1}, "example.sum"});
    auto group = executor.submit(std::move(tasks), poima::jobs::Lane::frame);
    auto result = executor.wait(group);
    result.rethrow();
    if (result.snapshot.state != poima::jobs::State::succeeded) return 1;
    std::cout << total << '\n';
}
```

Save it as `build/jobs-example.cpp`. To try the standalone scheduler on Linux
from the repository root:

```sh
c++ -std=c++20 -O2 -pthread -Iinclude src/jobs.cpp build/jobs-example.cpp -o build/jobs-example
./build/jobs-example
```

Success prints `42`. Engine consumers normally link `poima_core`; this command
exercises the scheduler alone, without simulation, rendering or profiler setup.
For failure handling, `result.rethrow()` exposes the lowest-index failing task.
A cancelled result has no required exception: inspect its state before publishing
output. A terminal group releases its admission capacity but retains inspection
metadata and any failure referenced by its handles.

## Integrated consumers

Animation samples independent rigs into candidate clock, layer-history and pose
partitions. Each rig preserves its serial math and ordered layer blending.
The owner waits, assembles poses in stable rig/node order, and commits histories
only after all work and output construction succeed. Failure or cancellation
discards the whole candidate. Save restoration retains the selected executor;
worker count is execution policy, not saved gameplay state.

Material jobs freeze their recipe and own their cancellation/output storage.
Workers bake and encode without file I/O. The service owner polls and publishes
checked image packages. One material bake may be active per service, with eight
retained records. Cancellation before publication vetoes even an already
completed CPU result. Closing the service cancels and joins its work without
shutting down another consumer's shared pool. See [procedural materials](PROCEDURAL_MATERIALS.md).

## Profiling

Start the [native profiler](PROFILER.md) before submitting work. Named
`animation.sample.rig` and `material.bake` events retain submission-time source,
session, tick and parent, actual CPU thread ID, group/task identity, queue delay
and measured execution interval. Worker rings are preallocated and bounded.
The owner merges completed records into the original active recording.
Stop, restart, destruction or capture overflow prevent late records from
entering another capture. Ring loss remains visible in `jobs.status`; it cannot
be reliably assigned to one capture when several share a ring.

Exports use actual CPU thread lanes. Logical sources remain metadata.
These intervals measure elapsed wall time, not sampled CPU utilization or
presented frame time. Physics uses its existing single-threaded Jolt adapter;
render recording, AI/navigation and general engine systems do not become
parallel merely because a scheduler is available.

## Qualification and timing

The [milestone evidence](evidence/m2-native-jobs.json) records native consumer,
sanitizer, compiled-game, simulation-disabled and Vulkan reference checks. It
also retains CPU animation timing summaries for an original fixture with 64
nodes per rig, three transform clips, two masked layers and interrupted
transitions. Every mode runs the same fixed ticks and compares actual pose
bytes and final saved state.

Observed native Windows sampling costs on the target Ryzen 7 8845HS laptop:

| Rigs | No-executor reference p50 / p95 | Two workers p50 / p95 |
| --- | --- | --- |
| 8 | 0.559 / 0.693 ms | 0.241 / 0.292 ms |
| 32 | 2.313 / 2.849 ms | 0.875 / 1.001 ms |
| 96 | 6.921 / 8.328 ms | 2.619 / 3.025 ms |

These are one run per OS, with 64 warmups and 128 measured samples per mode.
Mode order rotates each tick. Sampling includes candidate copies, scheduling,
rig evaluation, waiting, flattening and owner commit. It excludes command
application, fixture construction, verification serialization and the separate
profiler check; the evidence also reports command-plus-sampling costs and the
other worker counts. No-executor reference means the same candidate sampler
without an executor, not an earlier engine version. The six contexts share an
original model and remain alive together, with 15 mostly idle worker threads.
Memory readings cover that whole process. This does not establish real-character
throughput, CPU utilization, whole-game FPS, thermal behavior or a universal
speedup. The two-worker default remains a bounded policy rather than an
automatic hardware tuner.

To reproduce after building a simulation-enabled native configuration:

```sh
cmake --build build/runtime-headless --target poima-job-workload-benchmark --parallel 2
./build/runtime-headless/poima-job-workload-benchmark --iterations 128 --profile
```

For the Windows build, select `build/windows-runtime` and run its `.exe`.
Close other build/test workloads before interpreting timings. `--profile` adds
a separate four-tick observation after each workload; it does not instrument
the timing samples. The program exits nonzero if pose/save comparisons or the
required task/wait observations fail. It has no performance pass threshold.
