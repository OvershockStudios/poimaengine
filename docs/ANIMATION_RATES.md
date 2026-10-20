# Change animation speed without replacing playback

Use animation rate commands when movement speed changes during an animation
transition. They preserve the destination's current phase, the outgoing source,
the original transition deadline and any inertial correction. Base clocks and
configured layer clocks use the same operation. Layer weight ramps are separate
and remain unchanged.

Complete `animations` commands still replace playback. Use those to change a
clip, seek, resume a stopped clip or start a different transition. Rate commands
change only speed; they do not synchronize phases, fit strides or lock feet.
A deliberate rate discontinuity can change velocity even though the pose at
the command boundary remains continuous.

## Command service

Discover `runtime.step` and the installed `runtime_animation_rates` capability
before using this optional command field. Simulation must be built and running.
Supply the actual session and expected tick; after a structural transaction,
also supply its current `expected_structure_revision`.

```json
{"jsonrpc":"2.0","id":1,"method":"runtime.step","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","request_id":"cccccccccccccccccccccccccccccccc","expected_tick":120,"ticks":6,"animation_rates":[{"entity":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","speed":0.65},{"entity":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","layer":1,"speed":1.2}]}}
```

Each row requires `entity` and finite `speed` within `0..8`. Omit `layer` for
the base clock; an explicit layer must be a configured slot in `1..4`.
Commands apply before the first requested tick and share the complete batch's
rollback and retry receipts. Retrying the same retained request returns its
result without applying the rate or advancing again. A changed retry rejects.

Replacement and rate commands share a limit of 64 writes per tick, including
caller and compiled gameplay writes. They must target different clocks. A base
write and layer write on the same rig are independent; two writes to the same
base or layer reject. This includes a replacement combined with a rate change.
A same-speed write still consumes admission and target ownership, but leaves
the clock and its anchor unchanged.

`runtime.entity` and joined runtime observations report the effective animation
speed and phase through their existing playback fields. Reads do not sample,
advance, retime or expose uncommitted gameplay writes.

## C# gameplay

Declare `IAnimationRateGame` on the game class. It includes the existing
inertial-animation marker and enables both speed-only methods:

```csharp
var observed = context.GetAnimationExtended(rig)
    ?? throw new InvalidOperationException("Animation rig is absent");
context.SetAnimationSpeed(rig, Math.Clamp(measuredSpeed / nominalSpeed, 0, 8));
context.SetAnimationLayerSpeed(rig, 1, 0.5);
```

Compute and validate a positive `nominalSpeed` from game policy. A nominal
movement speed is not automatic stride calibration. Use `IMaskedAnimationGame`
as well when calling the existing layer getter or complete layer setter; the
rate feature alone grants speed-only layer operations.

Character input preparation precedes `Tick`, so `Get(entity).Velocity` can
already reflect prepared input. To measure movement actually achieved, sample
successive committed positions with a consecutive-tick guard and divide their
horizontal displacement by `DeltaTime`. Save that position/tick history with
gameplay state, as the test consumer and Locomotion Yard do.

These setters are valid during `Tick`. They stage writes for publication after
the callback; getters inside that callback continue to return committed state.
If an error escapes `Tick`, publication rejects, or a later tick fails, the
complete batch restores its original game, animation, physics and save state.
Game code can catch a rejected setter call; that does not cancel an earlier
accepted staged write. The test consumer
in [managed animation rates](../tests/managed_animation_rate_gameplay) measures
real character velocity and checks this staged-read boundary.

The named `animation_rate_v1` extension uses services epoch 7 with at least
376 bytes. Its versioned command is 40 bytes, with one appended setter at
offset 368. Earlier service prefixes retain their layout. A matching epoch
alone does not establish extension compatibility; use the negotiated feature,
extent and callback checks. CoreCLR and shipping-artifact qualification remain
separate evidence scopes.

## Phase, transitions and root motion

At a rate change, the destination retains its accumulated phase. Subsequent
fixed ticks advance from that boundary at the new rate. A zero rate holds the
destination while the transition continues toward its original ending tick.
Paused playback stays paused. Ended nonlooping clips stay ended, and rest or
zero-duration clips do not resume.

An outgoing transition clock retains its own rate. Root-motion intervals use
the retimed destination and preserve the outgoing clock or inertial velocity
correction. Consequently, zero destination speed during a fade can still
produce root motion. Full replacement later captures an outgoing retimed clock
or its evaluated interrupted pose through the existing transition policy.

The original destination command and anchor remain available for validating
inertial offsets. A bounded latest rate segment supplies the effective phase
and speed. Its internal unwrapped phase is distinct from an authored seek:
the existing `0..1e9` authored-time and root-motion bounds remain unchanged.
The internal segment uses a conservative finite bound derived from the maximum
exact simulation tick and maximum admitted rate.

## Saves and checks

Retained rate segments select nested animation-state version 9 with an explicit
`legacy_version` identifying the original layer, IK, root-motion and grounding
profile. Every clock then retains its history, including a clock that has not
yet used inertial playback. Cold restoration can therefore retain that history
for a later first inertial transition.

Without a retained segment, serialization keeps its original version and shape.
A moving outgoing source can retain a segment after its destination is replaced;
that active source still needs version 9. Existing versions 1–8 keep their
original validation. Version 9 additionally validates segment fields, original
transition anchors, inertial reference offsets and profile membership. Derived
hierarchy caches remain absent from saves.

Build `poima-runtime-animation-rate-test` and run
`ctest --test-dir build/linux-game -R '^runtime_animation_rate_native$' --output-on-failure`.
The executable also accepts `--output <absolute-new-directory>` to retain
complete save operands; its parent must already exist and the output must be
new. Six portable animation groups cover independent phase, crossfade,
quintic, layer, root-motion and save expectations. Simulation builds additionally
run the real Runtime partition, rollback and cold-continuation group. A portable
pass does not substitute for that native Runtime group.

The opt-in [original-character CLI check](../tests/runtime_animation_rates.py)
uses the unchanged Knight and license described in [character libraries](CHARACTER_LIBRARY.md),
the matched managed bridge and the compiled
[rate consumer](../tests/managed_animation_rate_gameplay). Supply a Linux
simulation CLI with `runtime_animation_rates`, a .NET 10 hostfxr library and a
new result directory whose parent exists. It builds and downloads nothing:

```sh
POIMA_JOB_WORKERS=2 DOTNET_PROCESSOR_COUNT=2 nice -n 10 python3 -B \
  tests/runtime_animation_rates.py --binary "$POIMA_CLI" \
  --source "$KNIGHT_GLB" --license "$KNIGHT_LICENSE" --hostfxr "$HOSTFXR" \
  --bridge "$BRIDGE_DLL" --assembly "$RATE_GAME_DLL" \
  --output "$FRESH_RESULT" --timeout 180
```

Set these paths to your actual artifacts. The consumer must include all 26
fields, including its committed-position history. The check retains original
requests, responses and complete save files on success or failure. It uses one
intact Knight and twelve standard analytic ribbon rigs to test 65 valid clocks
within existing world budgets. Its native-leader receipts are separate from
whole-process-tree qualification, which additionally needs a bounded supervisor.

The [0.0.201 results](evidence/m2-animation-rates.json) record 766 native rate
checks, 92 whole archive pairs including seven complete Runtime pairs, and
98 managed ABI groups. The separately supervised CLI workflow passes 218 RPCs,
31 complete checkpoints and 15 whole-file pairs across two native processes.
Its public reproduction script differs from the executed harness only in path
resolution and descriptive metadata; that adapter was source-reviewed.
These are headless control/state checks; rendered gait and foot locking remain
separate work.
