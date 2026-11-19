# Match animation phase at a transition

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use matching landmarks when replacing one looping clip with another. For a
walk-to-run transition, mark the left and right foot contacts in each clip.
Poima resolves the new start time from the outgoing clip’s position between
those landmarks. This lets the two motions start the blend on the same step
even when their timings and start poses differ.

The timing model follows the established technique of matching the fractional
position between corresponding clip landmarks. [Animation Sync Groups](https://dev.epicgames.com/documentation/unreal-engine/animation-sync-groups-in-unreal-engine)

## Make a transition

Discover the `animation_phase_matches` array in `runtime.step`.
Start a simulation with a positive-duration looping clip selected on the base
clock or the desired animation layer. Inspect the clip name and duration before
marking contacts. Send the new clip with its `loop` flag true and `time` set to
zero so the matching policy selects the start time.

```json
{
  "entity": "00000000000000000000000000000064",
  "clip": 48,
  "time": 0,
  "speed": 1,
  "loop": true,
  "playing": true,
  "blend_ticks": 12,
  "phase_sync": {
    "source": [0.11666666666666667, 0.65],
    "destination": [0.15, 0.55]
  }
}
```

Put this row in `animation_phase_matches` or inside the `animation` field of a
physical target command. For a masked layer provide its configured `layer` and
`weight` as with any replacement. Each array contains two to eight finite times
in seconds, with the same number of steps and the same event at each index.
Times are distinct and strictly inside the interval from zero (inclusive) to
the duration (exclusive). List each clip in forward cyclic order. For example,
`[0.7, 0.2]` is valid in a one-second clip if the first marker belongs before
the seam and the second is after it.

At the guarded command tick, matching uses the current clock’s effective phase,
including retiming. During an interrupted fade it uses the outgoing destination
clock. The normal crossfade or inertial policy still determines which pose moves
out of the blend. Paused source and destination clocks can be sampled. Both need
a selected looping clip with a positive duration. When the current clip has no
corresponding motion, issue an ordinary explicit start or use a dedicated start
clip.

Read `runtime.entity` or `runtime.observe` to inspect the destination’s playback
time after advancing. The result uses the existing clock shape. Saves store the
resolved start and ordinary transition state, so the annotation is unnecessary
to restore. An exact retry returns the initial receipt. A retry with different
landmarks is a different request. Malformed commands return `-32602`, and
invalid runtime clip or marker intervals reject the batch with `-32040`.

## Check the motion

At the same input, camera, character and speed, compare an ordinary clip-zero
transition with a landmark-matched transition. Inspect the foot that should be
supporting the body at the blend boundary, then check toe clearance, support
slip, cadence, knee motion, and weight transfer over the rest of the step.
A low point in an in-place source clip helps annotate a landmark. Measure
actual slip relative to the floor or moving support in the game.

The command aligns the start of the destination clip. The two playback rates
then advance independently. Continuous phase synchronization, automatic marker
extraction and adjustments to stride or pelvis motion require separate systems.
The marker command is exposed through native C++ and the service. Compiled C#
can still set a known start time through its existing animation controls.

## Reproduce the checks

Use a simulation-enabled Linux build and the original, pinned [Knight GLB](CHARACTER_LIBRARY.md#original-source).

```sh
cmake --build build/linux-game --target poima poima-runtime-animation-rate-test
ctest --test-dir build/linux-game -R '^(runtime_animation_(blend_contract|rate_native))$' --output-on-failure
python3 tests/runtime_animation_phase_knight.py --binary build/linux-game/poima \
  --source build/character-source/Knight.glb --output build/knight-phase-check
```

The imported-character check retains the input/reply stream and both toe-height
trajectories. It uses unchanged `Walking_A` and `Running_A` clips and compares
both sides of the gait. It measures the model-space start and blend effect
through native runtime transforms. World-space foot slip, the final look in a
rendered player, and other rigs need their own checks.

To record matched lower-body views, use a Windows Vulkan executable with
`--gpu <adapter-index>`. Add `--windows-interop` when invoking it from WSL.
