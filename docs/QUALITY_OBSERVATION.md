# Observe and verify game quality

<!-- SPDX-License-Identifier: Apache-2.0 -->

An agent needs enough evidence to find a defect, explain its cause, make a fix,
and check the result in the game. A playtest should combine the evidence that
answers the question. A grip issue needs the presented view and the hand and
item transforms. An audio dropout needs the mixer events, the samples and the
output-device timeline. This document sets the target for observation and
quality checks across Poima. The linked contracts describe what is available
now. Use [focused discovery](WORLD_SERVICE.md) on the running build.

## Follow one occurrence through the engine

When supported, record the build, project or game content identity, authored
revision, session, structure revision and simulation tick. Include the camera or
listener identity and frame or audio-sample interval when relevant. Attach
entity IDs and incarnations, asset hashes and job or request IDs to the
diagnostic rows that reference them. Distinguish proposed commands, accepted
requests, committed effects, render submission, presentation and device output.
Show the frame actually associated with an observation, or give the measured gap.

Use several channels where they add evidence:

- **Read state.** Request named fields and bounded history for the implicated
  object, including both requested and achieved values. Use measured values and
  named failure reasons so the agent can decide what to inspect next.
- **See the result.** Capture the intended player camera, then use another view or
  a diagnostic overlay for contact, bounds, joint frames, depth, motion, shadows,
  audio emitters or scene placement as appropriate. Keep rendered observations
  distinct from what reached the user’s display.
- **Hear the result.** Retain selected signals, cue IDs and mixer timings when a
  sound is in question. Observe the actual device for dropout and latency cases.
- **Trace the cause.** Record structured events, decisions, dependencies, rejected
  operations and counters by subsystem. Keep the request and the result together.
  Report omissions, sampling, saturation and dropped records explicitly.
- **Replay it.** Retain the checkpoint, inputs, assets and settings, including
  relevant seeds and clocks. Compare the original and corrected take with the
  same conditions. Use a fresh executable, process or exported game when the
  defect concerns lifecycle, packaging or save continuation.

Start with the [runtime observations](RUNTIME_OBSERVATION.md),
[presented UI queries](PLAYER_UI_OBSERVATION.md), [render diagnostics](RENDER_DIAGNOSTICS.md),
[spatial audio tools](AUDIO.md), [profiler](PROFILER.md),
[frame performance](FRAME_PERFORMANCE.md), and [asset records](ASSET_PROVENANCE.md).
Keep inspection read-only. Use explicit simulation steps or edits to test behavior.

## What to check

Some failures are unconditional, such as a nonfinite transform, dangling
reference, stale generation or impossible byte count. Others depend on the
authored rules. A floating prop is a placement defect if its placement policy
requires support. It can be valid while held, propelled or deliberately
suspended. Surface contact, volume containment, joint limits, camera clearance
and budgets need defined tolerances and coordinate spaces. Record the expected
and actual values, the scope and the reason if a check could not be evaluated.

| Area | Observation target | Checks to support |
| --- | --- | --- |
| Project and tools | Revision, proposed edits, request receipts, dependency graph, build phases and exact artifacts | Stale overwrite, duplicated retry, partial transaction, dangling instance/reference, wrong package, missing dependency or unreported truncation. |
| Import and content | Source/cooked mapping, nodes and materials, topology, bounds, transforms, skin weights, clip curves and tangent frames | Nonfinite values, invalid indices or weights, broken topology or bind pose, wrong scale/axes/winding, invalid material maps, unresolved source policy. |
| Scene and placement | Parent/local/world transforms, support surface, bounds, scatter seed, candidate rejection and collision shape | Hierarchy cycle, unintended overlap, floating or excessively buried grounded props, visible prop and collision disagreement, seams or regenerated moved props. |
| First-person body and held items | Camera, arm roots, head/torso/legs visibility, wrist and palm frames, grip sockets, aim, projection and final viewmodel transforms | Wrong hand or origin, grip drift, extreme wrist twist or limb stretch, duplicate hands or legs, head or weapon clipping, missing look-down stance, camera and aim coupling. |
| Third-person motion | Raw/fitted/solved pose, joint calibration, phase and weights, root/pelvis motion, support and both feet, posed skin and camera views | Out-of-range joints, discontinuities or pose flips, constraint stretch, contact sliding or penetration, leg crossing, bad load transfer, walk/run mismatch, limb swaps or hair/clothing clipping. |
| Physical contacts and interactions | Collider/filter state, query rays and hits, contact points/normals/impulses, body ownership, constraints and accepted action | Tunneling, phantom collision, unintentional gaps, penetration, stuck states, invalid mass/inertia, mismatched openings and colliders, duplicated response or impossible handoff. |
| Terrain, foliage and LOD | Tile identity and seams, height/normal and layers, support, wind field, biome/scatter state and LOD levels | Discontinuous surfaces, invalid slope assignments, detached stones or roots, LOD pops, collider/topology disagreement, unwanted disappearance of interactive state or footprints. |
| Lighting and materials | Light/probe/cache inputs, depth/normal/velocity, shadow caster/receiver and render history, reflections, AO and indirect paths | Light leaks, contact/shadow defects, invalid normals, phantom reflection, double occlusion, wrong history after a cut or reseed, material value or exposure errors. |
| Weather, water and environmental state | Weather seed and time, wind, wetness/snow, water surfaces and transfers, terrain state and persistent marks | Negative volume or lost water, visual/queried water mismatch, openings with incorrect flow, wrong material response, unrelated weather transitions, erased or duplicated marks. |
| Particles and effects | Causal event and emitter identity, seed, age/rate, positions, bounds, collision and overdraw | Duplicate triggers, detached effects, incorrect occlusion, tunneling or visually detectable temporal discontinuity, runaway particle count or flash/accessibility policy failure. |
| Camera and cinematics | Logical aim, presented transform, lens and view bounds, input and timeline/event state | Instant rotation, inconsistent aim, unmarked cut, invalid lens, lost action control, motion beyond comfort policy or missing transition/fallback. |
| Audio and acoustics | Cue event/priority, listener position, path/material, buses/peaks, mixing sample range, buffers and device outcome | Double triggers, looping or tiling artifacts, discontinuities, clipping, unexpected silence, invalid channel routing, incorrect source direction, timing or occlusion. |
| UI and accessibility | Displayed control tree, bounds, clipping, interaction target, focus order, settings and language | Offscreen/overlapped controls, focus traps or duplicate activation, invisible states, unreadable scaling, ambiguous text or unsupported contrast/motion policies. |
| NPC and living worlds | Inputs, decision trace, selected plan, target identity, navigation revision, reservations and time/resource transitions | Illegal knowledge, unreachable target, stuck movement, unbounded cost, duplicated inventory/commitments, lost persistence or nonrepeatable deterministic scenario. |
| Saves, replay and export | State schemas, clocks, asset/runtime IDs, save outcomes, checkpoints and export verification | Lost or incompatible state, applying an upgrade twice, reverted input settings, missing assets, divergence after restore or game dependency on the build workspace. |
| Performance, jobs and platforms | CPU/GPU phases, worker queues, frame and memory distributions, asset transfer/residency, devices and quality tiers | Deadlock, hanging work, stale results, unbounded resource growth, scheduling regressions, hitches, unexpected fallback, quality tiers changing gameplay. |

## Make the findings usable

Run inexpensive invariants where the data changes and capture the offending
identity immediately. Run contextual probes and visual comparisons on demand
or in a bounded replay. A single finding should name the observation, witnessed
value, applicable constraint, location/tick, and the nearest useful diagnostic.
Use stable codes and attach the relevant asset or native capture rather than
burying the evidence in a long console transcript.

Keep detailed logging opt-in and filterable by subsystem, entity and time.
Preserve performance, determinism and bounded memory while tracing. Limit the
amount of state an agent needs for each decision. Use a short summary to point
to exact samples. Label a visibly wrong result even when all numeric tests pass.
Use [discipline playbooks](PLAYBOOKS.md#choose-a-discipline) to select useful
observations, tests and recovery steps.
