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
cmake --build build/linux-game --target poima-runtime-physical-grounded-test poima-physical-controller-handoff-json-test --parallel 2
POIMA_JOB_WORKERS=2 ctest --test-dir build/linux-game -R '^(runtime_physical_grounded|physical_controller_handoff_json_native)$' --output-on-failure --parallel 1
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
