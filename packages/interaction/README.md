# Focused interaction

Reference `Poima.Interaction.csproj` from compiled C# gameplay. Set
`PoimaGameplayAssembly` to the matching SDK assembly when building outside
the engine source tree.

Call `FocusedUse.Probe` during gameplay Tick with the actor, the native
camera entity, a span of up to 64 eligible collider IDs and the reach in
metres (0.001–10,000). Use the actual collider entity as the target. The probe
casts one ray from the current camera position along its forward axis, ignoring
the actor. `Miss` means the ray hit nothing. `Blocked` retains the first
hit outside the target catalog. `Target` retains a hit on an eligible entity.
An empty catalog can still report the first blocker.

```csharp
using Poima.Interaction;

// Inside Tick, with the actor, camera and warning bell IDs from authored state.
var focus = FocusedUse.Probe(context, actor, camera, [bell], 2.5);
if (focus.IsTarget && context.Pressed(actor, GameAction.Use))
{
    // Change the objective and play the bell cue.
}
```

Use the same result to show the prompt and accept the input edge. Gameplay
owns availability, input, feedback and durable state. Test an in-range
hit, an intervening blocker and an out-of-range attempt with the actual
camera and collision shapes.
