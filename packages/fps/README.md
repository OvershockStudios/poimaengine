# Poima FPS feedback

Optional compiled C# helpers for procedural first-person feedback. Games own
all mutable state and decide which effects affect aiming or presentation.
The native engine does not install this package for unrelated games.

- `Feedback.Step`: analytic critically damped scalar/vector springs, carrying
  position and velocity; frequency in hertz, time in seconds.
- `Feedback.ApplyImpulse`: explicit velocity impulses.
- `Feedback.SwayTarget`: bounded look-delta response in radians.
- `Feedback.AdvanceBobPhase` / `EvaluateBob`: motion from achieved travel
  distance, with caller-owned phase and idle envelope.
- `Feedback.NoiseAt` / `Shake`: seeded smooth variation from explicit tick,
  cell interval and amplitudes; no wall clock or mutable RNG.
- `Recoil.AngularKick`: authored patterns, seeded random variation or their
  sum. Independent profiles/streams can drive aim, weapon kick and camera effects.

## Recoil profiles

```csharp
ReadOnlySpan<Vector3d> pattern = [new(.012, -.002, 0), new(.016, .003, 0)];
var kick = Recoil.AngularKick(pattern, shotIndex, seed,
    variationRadians: new(.003, .002, 0), mode: RecoilMode.Hybrid,
    patternEnd: RecoilPatternEnd.Repeat, stream: 0);
```

Axes are pitch, yaw, roll in radians. Entries describe individual accepted-shot
kicks, not absolute orientations. `Authored` ignores variation; `Seeded` ignores
the pattern; `Hybrid` adds both. A pattern has at most 256 samples. Only Seeded
accepts an empty pattern. Repeat wraps, Hold repeats the final sample, and Stop
returns zero after the end of an authored/hybrid profile. Callers decide when
a burst resets its shot index. The helper never advances it.

A kick can offset a target angle or supply a spring impulse. The example uses
separate aim and weapon springs. Camera shake and weapon motion never enter
its hit rays; [BaseWorld](../../docs/CAMERA_POSES.md) provides the unaffected
aim reference. Randomness is deterministic for the same recipe, shot, seed and
stream; no cross-platform floating-point replay guarantee is implied.

Inputs and outputs are bounded and validated. Invalid values throw before a
result is returned. Angular recoil is bounded to ±π/2 per axis; variation is
nonnegative. Hybrid sums outside that domain reject rather than saturate.
Read the XML API comments for each helper's remaining numerical bounds.

## Use and save state

Reference `Poima.Fps.csproj` from a trusted game project, or include its two
source files as the [FPS Feedback sample](../../examples/fps-feedback/README.md)
does. Builds require the same .NET toolchain and Poima gameplay SDK as the game.
The package has no native callbacks or generated art/content.

Save primitive spring positions/velocities, phase, envelope, shot index, seed
and time explicitly in `Game<TState>`. `ScalarSpring`, `VectorSpring`, `BobTarget`
and `NoiseCoordinate` are transient numerical records; they are not supported
nested game-state field declarations. No hidden state, normal-path allocation,
global random generator, reflection or wall-clock timing is used.

An authored camera [viewmodel-clearance profile](../../docs/VIEWMODEL_CLEARANCE.md)
can constrain the resulting foreground after pose interpolation, using current
physics. This native presentation step retracts the configured conservative
capsule or suppresses foreground draws when unresolved. It does not change the
package's saved spring state, desired effect offsets, world-camera effect or
aim rays. `rendered_viewmodel` still describes the desired offset; renderer
receipts contain the actual matrices after clearance. Fit the proxy to the
selected geometry; these helpers provide neither automatic mesh fitting nor
an outward clearance-recovery spring.

Procedural effects can layer over imported animation clips. This package does
not replace skeletal playback, IK, motion matching or an imported-character
pipeline, and does not implement a weapon inventory/attachment framework.
