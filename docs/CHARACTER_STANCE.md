# Character stance

Opt an authored `CharacterController` into native crouch/stand clearance with an
optional frozen `stance` profile. This changes its collision capsule, movement
speed and controlled camera height. Combine it with optional
[virtual locomotion](CHARACTER_LOCOMOTION.md) for stairs, slopes and supported
platform transport. Swimming, prone movement and a complete locomotion system
remain separate work.

## Author the profile

A complete controller value with a direct-child Camera can be:

```json
{
  "radius":0.3, "height":1.8, "speed":4, "jump_speed":5,
  "camera":"00000000000000000000000000000065",
  "stance":{"crouched_height":1.0,"crouched_speed":2.0,"camera_drop":0.8}
}
```

Use `camera:null` for a camera-free actor. All three profile fields are optional:
`stance:{}` selects the defaults shown above. Null, unknown fields, booleans in
numeric fields and nonfinite values reject. The crouched height must exceed
`2*radius` and be strictly below standing height. Crouched speed is `(0,30]` m/s;
camera drop is nonnegative and at most `height-crouched_height`, with a `1e-6`
allowance for numeric conversion. Values must also remain valid at native
float precision. Defaults can be invalid for a differently sized controller;
author a suitable complete profile in that case.

Omitting `stance` preserves the ordinary controller and its existing authored
serialization. The profile stays frozen through live authored edits and is
copied into hierarchical instances; it introduces no new entity references.
Existing controller camera references still remap to the instance's local
camera. Removing or changing frozen stance through a custom save-upgrade plan
is not authorized.

The collision shape keeps the actor's foot origin and radius. Physical contacts
still resolve under Jolt. A stance-controlled camera must be an existing direct
child Camera without `RigNode`: animation and stance cannot both own its
position. Camera X/Z come from its initial local position; Y is the initial
height minus the actual crouched drop. Standing restores the initial height;
repeated commands do not accumulate offsets. Controller pitch retains its
existing rotation ownership. Camera-free actors use the same collision policy.

## Request and inspect

Schema revision 93 adds `character_stances` to `runtime.step`:

```json
{
  "session_id":"<session>", "request_id":"<request>",
  "expected_tick":120, "ticks":1,
  "character_stances":[{
    "entity":"00000000000000000000000000000064", "crouch":false
  }]
}
```

Supply the usual `expected_structure_revision` after a membership change.
Each row requires a live configured controller and a strict Boolean `crouch`.
At most 32 unique targets are allowed, combined with gameplay writes for the
first Tick. Caller commands apply on that Tick; omitted commands retain desired
stance. Identical retained retries return their original result; conflicting
parameters reject. A failed Tick or later Tick restores the entire batch.

Crouching shrinks the capsule at its foot origin. Standing tests the actual
physics shapes before growth. An obstruction is a successful committed Tick:
`requested_crouch:false`, `crouched:true`, `stand_blocked:true`. The actor keeps
crouched speed and camera height, and retries standing on subsequent Ticks
without another setter. Supporting-floor contact slop is handled separately
from ceiling/wall clearance. Jump, gravity and collision resolution remain
under the existing character physics policy.

Query without advancing or retrying clearance:

```json
{"session_id":"<session>","tick":121,
 "entity":"00000000000000000000000000000064"}
```

Send those parameters to `runtime.character_stance`. Its `state` contains
`entity`, `ground`, `configured`, `requested_crouch`, `crouched`, `stand_blocked`,
`radius`, `standing_height`, `crouched_height`, and the effective `height`,
`speed`, `camera_drop`. Ground uses the existing `on_ground`, `steep_ground`,
`not_supported`, `in_air` values. A live noncontroller returns null; a missing
entity rejects. A legacy controller reports `configured:false`, standing height
and speed, zero drop and neutral stance flags; it cannot receive stance writes.
Session/tick guards are exact. Inspection never changes posture or simulation.

## Compiled gameplay

Declare `ICharacterStanceGame` independently of character input or animation.
The example below also declares named-input access; author a `crouch` binding in
the player's input profile rather than assuming a default key:

```csharp
using Poima;

public struct StanceState { public EntityId Actor; }

[GameModule("example.stance")]
public sealed class StanceGame : Game<StanceState>,
    ICharacterStanceGame, IInputActionsGame
{
    public override void Initialize(ref StanceState state)
        => state.Actor = new(0, 100);

    public override void Tick(ref StanceState state, GameContext context)
    {
        CharacterStanceState? committed = context.GetCharacterStance(state.Actor);
        if (committed is null) throw new System.InvalidOperationException("Controller absent.");
        context.SetCharacterStance(state.Actor,
            context.GetInputAction(state.Actor, "crouch").Held);
    }
}
```

Getters read callback-entry committed state; staged setters remain invisible
within that callback. There are at most 256 admitted reads and 32 distinct writes
per Tick. Caller/gameplay writes share the latter budget and cannot target the
same controller. Configured pending births expose their authored standing state
and accept staged stance; commands targeting a removed member reject the batch.

`character_stance_v1` requires the independently negotiated 336-byte services-7
prefix. It does not grant `ICharacterInputGame`, camera-lens or other marker
rights. Call ABI 1/80 remains unchanged. CoreCLR is the compatible reload path;
Native AOT libraries remain pinned for their process lifetime.

## Checkpoints and limits

Any frozen stance profile, including an unused hierarchical recipe, selects
runtime snapshot version 8 when no virtual locomotion profile is present.
[Virtual locomotion](CHARACTER_LOCOMOTION.md) selects version 9 and retains the
same posture state alongside its logical motor state. Version 8 retains the complete version-7 payload, with an
empty cameras array when appropriate, and adds strict posture flags only to
configured controllers. Restore installs the saved capsule before restoring
body poses and reconstructing the camera. Older snapshots default configured
controllers to standing; worlds without stance retain their previous format.

Complete in-process batch rollback includes capsule ownership, intent, actual
stance, blocked status, eye height, physics, gameplay, membership and save
intents. Durable saves reconstruct Jolt contacts rather than serialize solver
caches. Exact cold-to-cold continuation is separate from uninterrupted warm
contact trajectories. Root-motion horizontal ownership and its input conflicts
remain unchanged; stance does not resample or consume a locomotion interval.

## Reproduce the contracts

Build the matching native engine and `poima-runtime-character-stance-test` using
the [runtime build instructions](RUNTIME.md#build). Windows native execution must
run beneath an external owned process-tree Job with deadlines and natural
cleanup; these commands do not establish that owner themselves.

```sh
python3 tests/runtime_character_stance.py \
  --binary /path/to/poima --output build/stance-protocol-new

python3 tests/character_stance_gameplay_contract.py /path/to/poima \
  --hostfxr /path/to/hostfxr --bridge /path/to/Poima.ManagedBridge.dll \
  --assembly /path/to/Poima.ManagedCharacterStanceGame.dll \
  --output build/stance-coreclr-new
```

Compile `tests/managed_character_stance_gameplay/Poima.ManagedCharacterStanceGame.csproj`
against the matching SDK first. For a published Native AOT consumer, replace the
three managed paths with `--native-descriptor /path/to/native-gameplay.json`.
Use fresh output directories. These fixtures contain synthetic physical and
save oracles; qualification results belong in the implementation status and do
not follow merely from the existence of the tests.
