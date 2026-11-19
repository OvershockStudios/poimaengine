# Carry Props

This compiled C# example combines aimed pickup, carrying and dropping in one
repeatable first-person interaction. Look at the prop, move within reach and
pick it up. The wrist approaches the calibrated grip and the fingertips must
reach the prop before its world collider retires. The hand returns to its
carrying pose. Dropping opens the fingers and asks physics to admit a new
body at the held pose. If a collider blocks the release, the player keeps
the item and the hand closes again. Once the item settles, it can be picked
up again.

The optional HUD label reports the current action or refused drop. It reads
the same committed physics and attachment results as agent queries. Set its
artwork through the authored [game UI image styles](../../docs/GAME_UI.md#use-cooked-images).
The example's HUD and geometry are original analytic fixtures.

## Build the sample

Use a simulation build with the current gameplay SDK. Windows capture and
play need Vulkan and native game UI. Publish
`Poima.Examples.CarryPropsGame` from `Poima.CarryPropsGame.csproj` with
[the Native AOT publisher](../../docs/NATIVE_GAMEPLAY.md).
From a native Windows shell with the Python dependencies available, run:

```sh
python tests/carry_props.py --binary <poima.exe> --descriptor <native-gameplay.json> --output build/carry-check --runtime <installed-windows-runtime>
```

Add `--windows-interop` when invoking the Windows executable from WSL.
Add `--gpu 1` or another explicit device index to retain native captures.
The verifier exports and relocates a standalone game into
`build/carry-check/delivery/Relocated Carry Props`. Open its Windows launcher
to play. Move with WASD and look with the mouse. Press E to pick up, Q to cancel
a reach and G to drop. The export path tests those semantic actions through
the standalone game's service. Physical input is a separate check.

For CoreCLR, build this project and the bridge with .NET 10. Extract the
[component manifest](../../docs/CUSTOM_COMPONENTS.md#import-and-author)
from `Poima.CarryPropsGame.dll`. Pass `--assembly`, `--bridge`, `--hostfxr`
and `--manifest` in place of `--descriptor`. Omit `--runtime` for the
source-world workflow.

To replay the same actions through the Windows player, use
[`game run --replay`](../../docs/PROJECTS.md#run-or-replay) with the bundled
manifest. Convert the `runtime.step` requests in
`delivery/warm/stdin.ndjson` to its semantic segments. For each step copy
`ticks` and the single actor's `move`, `look` and `actions` fields.
Include `motions` where present. The player supplies the actor from the
manifest entry.

## Adapt the interaction

Import the component manifest and attach `CarryPropSettings` to the
controller in the stopped authored world. Use the same rig, finger layer,
receiving socket and [calibrated grip settings](../reach-pickup/README.md#author-the-interaction)
as the basic reach example.

| Field | Configuration |
| --- | --- |
| `Camera` | Controller's aim camera. The hand rig can have a different rotation and offset. |
| `TemplateHigh`, `TemplateLow` | The two 64-bit words of the release template's ID. The template must produce a single root with a fitted dynamic BoxCollider. |
| `PromptHigh`, `PromptLow` | The authored label's ID words, or both zero for no HUD prompt. |
| `WindupTicks`, `RecoverTicks` | Finger opening and recovery durations. The sample uses 7 and 9 ticks. Valid ranges are 1–120 and 2–120. |
| `StartingAnchor` | A fixed live entity used to place the startup prop. Supply `Item` and `Phase: -1` to enable placement. Give its box clear space above a stable support. |

When authoring through the service, int64 words use signed decimal strings
with the same bit pattern as the corresponding identity word. The native
verifier shows the complete field-ID mapping and starts the prop through
compiled gameplay, so a packaged player needs no setup RPC.

Props available to this game share a grip calibration and release-box
template. The sample drops from rest with zero initial world velocity.
Its aim checks visibility and its contact probes validate the grip.
An authored clear route for the arm and a calibrated carried rest pose are
still required. [Verification](../../docs/evidence/m2-carry-props.json)

## Check the arm corridor

Publish `Poima.Examples.ClearanceCarryPropsGame` from the same project to
probe the arm before applying each new reach and return pose. If the
corridor is blocked, the hand abandons that reach and the HUD says
“Reach blocked.” The prop stays in the world. A blocked return keeps the
prior IK controls and retries on later ticks. The sample uses a 2 cm capsule
radius for each segment of its small analytic rig.

Use `tests/ik_clearance.py` with the same CoreCLR or Native AOT paths.
The test positions a kinematic obstacle beside the center ray. It verifies
that the prop is still aimable, compares the predicted joint locations with
the native pose, tests both arm segments and loads a save taken during the
blocked reach. The same item can be picked up after the obstacle moves.
With Windows Vulkan, `--gpu` captures the blocked and recovered states.
[IK clearance](../../docs/TWO_BONE_IK.md#probe-a-reach-for-obstacles)

## Recheck before picking up

Publish `Poima.Examples.FinalClearanceCarryPropsGame` to add the arm check
inside the pickup transaction. The preview still filters each reach
pose. When an obstacle moves into the arm after the final preview, the
pickup keeps its release body and reports the blocker. The hand begins
its return when gameplay reads the result.

Run `tests/final_pickup_clearance.py` with the same build paths. It moves
the wall on the exact pickup tick, saves and restores the refusal, then
verifies that another attempt succeeds once the wall clears. To compare
with the 0.0.267 behavior, supply `--previous-binary` and either the old
Windows `--comparison-descriptor` or the matching Linux
`--comparison-assembly` and `--comparison-bridge`.
[Pickup clearance contract](../../docs/ITEM_ATTACHMENTS.md#recheck-the-arm-after-movement)
