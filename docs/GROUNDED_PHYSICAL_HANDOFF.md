# Grounded animation to physical takeover

Add `"procedural_pose": "grounded_animated_only"` to a linked
[`PhysicalRig`](PHYSICAL_RIGS.md#controller-handoff) to transfer a character's
committed ground-corrected pose into passive physics. Optional
[planted-foot anchors](FOOT_PLANTING.md) use the same ownership boundary.

The rig starts `animated`, has a `controller_handoff`, and contains at least
one configured automatic-grounding slot. Manual slots may coexist. Every
configured chain's root, middle and end must map to physical parts. The linked
actor is an unscaled root virtual-locomotion controller; its rig is a direct
child. Keep cameras on separate child entities. Root motion, additional moving
body owners and independent procedural writers in that subtree remain outside
this policy.

The existing `animated_only` policy remains manual-only, including its allowance
for zero configured IK slots. Omitting both policies retains the earlier
ownership checks. Disabled authored grounding still counts as a grounded slot;
it does not bypass mapping or ownership validation.
Existing grounding metric bounds, canonical support identities and the
two-grounded-foot limit still apply.

## The completed pose owns the transition

At each completed animated tick, source clips and layers compose first. All
foot decisions inspect that same uncorrected pose. The existing grounding
solver prepares controls, IK applies them, and grounding records final
residuals. Only then is the physical pose history recorded.

Takeover consumes this completed pose, including its finite-difference linear
and angular motion. It does not add controller or platform velocity a second
time. A newly born actor must first complete a locomotion tick. Requested
animation, rate, IK, root-motion or grounding writes combined with activation
on that owner reject the entire batch, including identical replacements.

Requested grounding enable/weight multipliers remain separate from generated
IK targets and weights. Authored limits remain their ceiling. Manual IK writes
to an automatically owned foot continue to reject.

During a gameplay callback, node and IK point/orientation previews can apply
the last committed generated controls under the prepared actor frame. This
scratch preview does not probe contact surfaces, advance a planting latch or
publish requested controls. Its grounding/contact fields still describe the
last completed decision; they are not a new contact solve under the preview.
Staged setters become visible at the regular successful command boundary.

## Inspecting a physical character

After takeover, physics owns the mapped bones. Source/layer clocks and raw
source scales continue, while that rig's grounding preparation, IK correction,
planting transitions and residual calculation stop. Other animated rigs keep
evaluating normally.

Use `runtime.procedural_pose` with exact `session_id`, `tick` and rig `entity`
to distinguish `phase: "animated"` / `diagnostics: "current"` from
`phase: "physical"` / `diagnostics: "last_animated_pose"`. `sampled_tick` is
the original completed animated boundary. Existing `runtime.ik` inspection
with `include_grounding` and `include_planting` retains the complete cached
foot decisions and anchors. A corpse's old end position, hit, target, casts
and residuals must not be read as a fresh solve on its current physical pose.
Grounding setters on a physical owner reject until a recovery policy exists.

The support may move or retire after takeover, including retirement in the
activation batch. Historical anchors retain their captured support-relative
frame and identity. They do not reacquire a replacement body, follow the
corpse, or require the old platform to remain in the live world.

## Complete saves and restore

Frozen content with this policy, including dormant templates, selects physical
snapshot extension 4. Every rig row includes `procedural_capture`; it is null
for other policies. For an animated grounded owner it stores the current
completed capture, allowing immediate takeover after a cold load. For a
physical grounded owner it exactly matches `procedural_terminal`.

The grounded witness uses `poima.procedural-terminal` version 2. It retains
one rig's complete historical animation context, wrapper, final node poses,
complete IK/grounding/planting diagnostics, requested policy, actual preceding
generated controls and requested multipliers, previous decisions, and a bounded
ordered query journal.
The journal records literal ray and support-query inputs/results, with native
support body IDs, motion kinds and unscaled rigid frames captured during
the original final sample. Each reply is consumed exactly once by a pure
replay of the existing prepare/solve/finish algorithms. Extra, missing or
inconsistent replies reject.

Startup and birth captures before the first contact decision retain exact
uninitialized states and an empty journal. They cannot stand in for a
completed grounded takeover pose. No saved cache is retimestamped to make a
restore pass.

Cold restore installs a physical owner's historical ownership before any
constructor sampling. It validates the captured decision at its original
tick, then joins current source clocks, controls and native physical state
separately. An animated capture must instead match the complete current
animation context, current native actor wrapper and live support bindings.
Other live feet keep their ordinary current-tick and current-support checks.

The reader decodes and validates every original state and pose before
publication. For grounded captures, a small, typed allowance covers only
specified recomputed pose/diagnostic values; complete input, control, query and
ownership checks remain exact. Admitted values are retained, including an
immediate re-save and rollback after a rejected operation. The host's validation
solve does not replace the original capture. See
[grounded save compatibility](GAMEPLAY_SAVES.md#grounded-save-compatibility)
for the numeric limits and restore-tick/source-history rules. Locally recorded
captures and manual terminal version 1 keep exact replay validation.

Surviving historical supports must retain the same native ID and eligible
motion kind. Retired supports are checked against original/allocated entity
identities and the used native body-generation ledger. An ID currently owned
by another live entity cannot substitute for a retired support. Current transforms are
not substituted for captured historical frames. These checks and journal
replay prove consistency with recorded observations; they do not reconstruct
an arbitrary past broadphase or prove that an old retired surface was the
nearest possible ray hit.

Existing worlds keep extension 1, 2 or 3 and manual terminal version 1 when
this new policy is absent. The enclosing complete snapshot remains bounded
to 64 MiB. Existing C# services-prefix and ABI epoch are unchanged; native
C++ configuration consumers must rebuild.

## Boundaries

This policy coordinates existing deterministic grounding and one-way physical
ownership. It does not add gait fitting, pelvis balance, active ragdoll
locomotion, get-up/recovery or root-motion transfer. Imported-character visual
quality, broader character families, representative performance and each
shipping platform require their own qualification.

## Run the native checks

With a configured simulation-enabled build and `POIMA_ENABLE_NATIVE_GAMEPLAY=ON`:

```sh
cmake --build build/linux-game --target poima-runtime-physical-grounded-test poima-runtime-physical-ik-test poima-procedural-replay-numeric-test poima-physical-controller-handoff-json-test poima-physical-grounded-gameplay-fixture poima-physical-ik-gameplay-fixture --parallel 2
POIMA_JOB_WORKERS=2 ctest --test-dir build/linux-game -R '^(runtime_physical_grounded|runtime_physical_ik|procedural_replay_numeric|physical_controller_handoff_json_native)$' --output-on-failure --parallel 1
```

CTest passes the actual compiled C++ fixture library to the runtime test.
Disabling native gameplay omits that callback lane. No C# or Native AOT
compiler is involved in this fixture.

Set `POIMA_TEST_ARTIFACTS` to a fresh output directory to retain complete
snapshots, negative inputs and a literal SHA manifest under its
`physical-grounded` subdirectory, with a 128 MiB recording cap. Reusing a
directory containing its manifest rejects rather than replacing earlier
records. The checks exercise new `Runtime` objects within one process;
fresh-process and other-platform qualification remain separate.

The [0.0.227 evidence](evidence/m2-grounded-physical-handoff.json) records a
passing selected 50-test Linux cohort. Its actual C++ callback fixture passes
ten feature groups and 7,895 checks. Independent review reopens all 324 complete
archive files, including twelve negative inputs, and all 119 complete warm/cold
pairs. Seven noncontact scenarios cover disabled and zero-weight requests,
lifted feet, actual misses, correction and slope rejection, and lost support.
Original foreign-owner and raw-normal acceptance failures remain retained;
the corresponding unchanged assertions pass after the fixes.

The [0.0.230 restore checks](evidence/m2-grounded-save-portability.json) add
complete typed-codec corruption cases and adjacent-double preservation tests.
The latter deliberately construct three small rounding changes and check full
state after load, a rejected multi-tick step, a rejected structural edit and
reopening. These are synthetic regression witnesses, separate from actual
cross-platform saves. Earlier grounded and manual-IK assertions remain intact.

To check an actual platform-produced character save, retain the successful
output of [the original-character workflow](#original-knight-workflow).
Copy its complete consumer directory, including `evidence.json`, `primary/`,
world assets and save slots, to the destination without editing their bytes.
Run the [portability test](../tests/physical_grounded_save_portability.py) under
an external process-tree supervisor with the destination CLI:

```sh
python3 -B tests/physical_grounded_save_portability.py \
  --cli path/to/destination/poima \
  --source-report path/to/copied-consumer/evidence.json \
  --output build/grounded-save-portability
```

The output directory must be new. Windows uses its Python executable and
destination `poima.exe`. The test maps recorded paths within the copied
consumer directory without rewriting the report. It checks all sixteen source
archives, immediate whole-snapshot preservation, atomic rejection and repeated
destination-local continuation, plus five fresh-process states. Passing requires
six naturally closed engine processes, ninety output archives and seventy-four
complete byte comparisons. `report.json` retains failures and literal requests
and responses. It builds nothing and opens no renderer or window.

## Compiled C# controls and fresh-process saves

A game implementing `IGroundingControlGame` and `IPhysicalRigGame` can inspect
physical ownership and stage grounding controls on an animated rig. For example,
reduce a requested foot multiplier without taking ownership of its automatically
generated IK target:

```csharp
if (context.GetPhysicalRig(state.Rig)?.Mode == PhysicalRigMode.Animated)
    context.SetGroundingControl(new(state.Rig, 1, true, 0.75));
```

Activation and grounding/IK writes for the same rig must occur in separate
Ticks; a conflicting batch rejects atomically. Getters within a callback observe the committed
requested controls, while staged writes publish at the supported boundary.
The multiplier is a request, not a guarantee of contact or achieved IK weight.

`GetGroundingControl` exposes `Enabled` and `Weight`. It does not expose the
contact or planting cache. Use `runtime.ik` with `include_grounding` and
`include_planting` for those complete native diagnostics. After takeover, the
point/oriented IK diagnostics retain the last animated pose; physical parts
and bone transforms describe the current solved body instead.

The [compiled fixture](../tests/managed_grounded_handoff_gameplay/README.md)
combines two planted feet, a translating/rotating support, a linked controller
and an independent live rig. It checks staged controls, prepared callback
observations, whole-state rejection, both support-retirement timings and native
body-generation reuse. A surviving game-held live `EntityId` must be cleared or
rebound when its entity is despawned; the fixture clears it. The native historical
contact may still retain that old identity. These are different lifetimes.

The [0.0.228 Windows/CoreCLR record](evidence/m2-grounded-physical-handoff-windows.json)
records separate Windows native and Windows/Linux CoreCLR execution. Each
managed run retains all 116 typed fields, 46 complete saves and 27 complete
byte/typed-state pairs across three sequential engine processes. This extends
fresh-process evidence beyond the same-process C++ fixture; it does not qualify
Native AOT, imported-character motion, rendering or representative performance.

## Original Knight workflow

[Physical Character Yard](../examples/physical-character-yard/README.md#planted-feet-into-passive-physics)
applies this policy to the unchanged CC0 KayKit Knight: all 57 original nodes,
fifteen physical bodies and fourteen joints remain present. Its bounded-scale
profile keeps the original animation scales and adds the explicit controller
and grounded-pose ownership links.

The workflow pauses Idle, obtains two planted contacts on a real flat collider,
and advances the controller about 5.33 mm while preserving both anchors. It
checks the corrected two-sample pose history, transfers ownership to passive
physics, and restores animated and physical checkpoints in fresh processes.
Complete archive equality covers rejected conflicting writes, restoration and
continued simulation. Cached foot diagnostics remain tied to the last animated
decision after takeover.

The [original-character evidence](evidence/m2-character-grounded-handoff.json)
records separate Linux and Windows CPU passes using the unchanged 0.0.228 native
runtime, schema 127. Each retains sixteen complete archives and eleven whole
byte/typed-state comparisons across three naturally closed engine processes.
This is imported-character integration coverage for the existing policy.
Paused Idle and a small motor displacement do not establish gait fitting,
active balance, fall animation quality or representative performance.

The original 0.0.229 Linux-to-Windows grounded restore rejected at exact generated-IK
replay validation. Separately produced checkpoints contain orientation
components differing by 1–4 floating-point ULP. Those within-platform passes
remain separate from the [0.0.230 save-portability checks](evidence/m2-grounded-save-portability.json)
and the [reader contract](GAMEPLAY_SAVES.md#grounded-save-compatibility).

## Depth reference and hardware capture

After the CPU workflow succeeds, a pure source-geometry reference can be made
from its retained `evidence.json`:

```sh
python3 -B tests/character_grounded_depth_oracle.py \
  --source /path/to/Knight.glb \
  --cpu-report build/physical-character-grounded/evidence.json \
  --output build/physical-character-grounded-depth.json \
  --require-stale passive-next
```

The output must be new. For Windows hardware captures, use the Windows CPU
run's saved poses. The reference joins the original model to the cooked
asset and saved node poses, then evaluates skinning, rigid attachments and
nearest-depth occlusion in software. It selects stationary planted, moving
planted, first-physical and later passive poses. The required stale case must
provide separated depth witnesses; insufficient separation fails that
requirement rather than lowering its threshold.

This file reports `native_render_verified: false`: it is an expected-depth
reference, not a Vulkan capture. Hardware verification separately needs actual
depth samples and submitted/presented-frame receipts from the identified GPU,
unchanged complete saves around capture, and retained images. Still-image or
depth agreement alone does not establish believable motion.

The separate [Windows Vulkan result](evidence/m2-character-grounded-handoff.json)
passes those checks on an RTX 4070 Laptop GPU for five Windows-saved poses.
Each pose's complete image matches after a fresh-process restore, and complete
saves remain equal to the original Windows checkpoints before and after
capture. The 640×480 single-sample images qualify selected still-frame depth
and replay agreement. Normal-speed motion, gait, active balance, antialiasing
quality and performance require separate evaluation.
