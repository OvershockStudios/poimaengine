# Relay Yard

A small first-person game assembled through Poima's native authoring API. Recover
three power cells, follow the animated courier around the cover, then activate
the relay terminal. Pickups and completion require an in-range camera ray and
the Use action; menu buttons cannot complete the objective.

The scene uses authored primitive geometry and the original licensed Kenney
Animated Characters Protagonists 1.1 body, Idle and Run FBX files. The
[Locomotion Yard import pipeline](../locomotion-yard/README.md) supplies source
inspection, explicit retargeting, the controller/visual attachment and CC0
provenance. No source asset is rewritten by this launcher.

![Native Relay Yard menu and imported courier](../../docs/evidence/relay-yard.png)

Native renderer readback from the exported game, with its compiled settings
menu open. Character source: Kenney Protagonists 1.1, CC0-1.0.

## Build the gameplay module

Run the publisher with a matching-host .NET 10 SDK and native linker. On Windows,
use a developer shell with the C++ toolchain available. Use new output and work
folders:

```powershell
python scripts/publish_native_gameplay.py `
  --project examples/relay-yard/Poima.RelayYardGame.csproj `
  --type Poima.Examples.RelayYardGame --rid win-x64 `
  --output build/relay-yard-artifact --work build/relay-yard-publish
```

The project compiles the existing locomotion component declarations alongside
this game's module. The publisher selects `Poima.Examples.RelayYardGame` and
emits its Native AOT library, component manifest, descriptor and dependency
notices. The sample needs native navigation, character input, inertial animation
and player-preference services. It does not add a second movement authority or
interpret a gameplay script at runtime.

## Author and play

Supply your original downloaded Kenney Protagonists 1.1 folder. The launcher
checks the exact supported source hashes before importing. The engine executable
must include the renderer, physics, navigation, FBX intake and native gameplay.
Run native Windows Python for the Windows executable:

```powershell
python examples/relay-yard/run.py `
  --binary build/windows-runtime/poima.exe `
  --source-directory C:/Assets/KenneyProtagonists `
  --manifest build/relay-yard-artifact/game.poima-components.json `
  --descriptor build/relay-yard-artifact/native-gameplay.json `
  --audio-directory examples/relay-yard/audio `
  --output build/relay-yard-play --gpu 1
```

Choose the GPU using actual device discovery. `--author-only` imports and authors
the world without launching the game. CoreCLR development is also available:
replace `--descriptor` with `--hostfxr`, `--bridge` and `--assembly` paths built
from this project and the managed bridge. Original content is supplied by the
caller; the launcher does not download assets or compile code.

## Optional licensed audio

`--audio-directory` enables the included five Kenney CC0 clips: two concrete
footsteps, pickup confirmation, denied interaction and relay confirmation. Eight
permanent emitters provide pickup, denial, arrival and win feedback, plus two
alternating footsteps each for the player and courier. Permanent pickup emitters
keep the cue alive when its collectible despawns. Omit the option for silence.

Footstep cadence follows committed native horizontal movement on the sample's
flat floor. A short support ray and vertical-motion checks prevent airborne
steps; stationary input, blocked movement and discontinuous position samples
cannot advance cadence. The sample caps steps per tick and saves each
character's phase and variation in its native `RelayAudioCadence` component.
It is a displacement-based sample, not animation-driven foot contact or a
general material-aware gait system.

The [audio manifest](audio/manifest.json) records original files, CC0 licenses,
source hashes, converted WAV hashes and the offline conversion recipe.
[prepare_audio.py](prepare_audio.py) reproducibly decodes, downmixes and resamples
the pinned originals to native 48 kHz mono PCM16 using recorded decoder and
resampler builds. Those tools are only needed to prepare content; playing the
included WAVs needs neither decoder library. Other converter builds can produce
different bytes; the launcher checks the published manifest and file hashes.

Newly compiled modules register the cadence schema even when the silent profile
contains no cadence instances. Enabling audio also changes the authored scene,
assets and component instances. These worlds are a separate content cohort from
the retained 0.0.86 save-upgrade fixtures: there is no implicit conversion of
those checkpoints into an audio-enabled world.

The originals come from Kenney's [Impact Sounds](https://kenney.nl/assets/impact-sounds)
and [Interface Sounds](https://kenney.nl/assets/interface-sounds). Their unchanged
licenses are in [sources/impact](audio/sources/impact/License.txt) and
[sources/interface](audio/sources/interface/License.txt). To reproduce the
published set with the recorded native libraries and original archives:

```bash
python examples/relay-yard/prepare_audio.py \
  --impact-archive /Assets/kenney_impact-sounds.zip \
  --interface-archive /Assets/kenney_interface-sounds.zip \
  --output build/relay-audio-reproduction
```

Preparation has no network requests. It rejects changed originals; the launcher
rejects changed clips, licenses or manifests before native import. Export carries
the referenced cooked audio and source/license/conversion credits, rather than
the original decoder tools or artist files.

## Controls and checkpoints

Begin starts the shift. WASD moves, the mouse looks, and E uses the closest
collider along the camera ray within three metres. Tab releases the cursor and
pauses native playback. Open Menu to save or load a checkpoint, inspect current
preferences, or change FOV, UI scale, pointer sensitivity and master gain. Close
resumes; click the scene to capture mouse look again.

The native owner controls pause. The welcome screen gates the game's logic;
the standalone player's clock and neutral physics can advance before Begin.
The game has no separate saved pause flag. A restored Runtime is paused by the
player; close a restored menu to resume. Welcome also offers Load for continuation
in a fresh process.

Checkpoints retain collected cells, courier route and animation, controller
state, UI and registered game state. Repeated saves resolve the current slot
generation through the native save service. Save/load errors are visible through
status text and the typed result. Current player preferences remain owned by the
current player; loading a checkpoint does not apply an old preference owner or
configuration. The sample's preference changes are session intent, with no
implicit settings-file write.

## Updating a saved game

The state declares a persistent ID for each field. `CheckpointVersion` has the
literal persistence default 2 and ordinary initialization also sets it to 2.
These are separate operations: defaults map newly added fields during an
authorized upgrade; `Initialize` prepares a new game. Keep each ID when moving
or renaming its field. Changing its kind requires a different migration policy.

The 0.0.85 sample used legacy name/layout metadata. Loading its checkpoint with
a rebuilt module remains an error without an explicit, hash-bound upgrade plan.
The trusted host selects both the target artifact and the plan; saved data never
selects executable code. After verifying the upgrade, write a new checkpoint
and retain the source save and artifact. The compiled menu uses ordinary exact
loads; it does not silently approve game updates. See
[the retained-game workflow](UPGRADING.md) and [save upgrades](../../docs/SAVE_UPGRADES.md).

## Qualification

This is a small integration workload, not evidence of AA/AAA scale or production
performance. [Implementation status](../../docs/IMPLEMENTATION_STATUS.md) records
which checks have run. Physical-device interaction, audible output, representative
performance and clean-machine delivery require separate qualification.

The semantic verifier plays the game through native movement, camera look and
Use inputs. It checks the welcome gate, failed missing-save load, wrong and
out-of-range rays, premature terminal use, real despawning pickups, repeated
compiled saves, exact partial restoration, stale guards, courier travel and
final completion:

```powershell
python tests/relay_yard_contract.py `
  --binary build/windows-runtime/poima.exe `
  --manifest build/relay-yard-artifact/game.poima-components.json `
  --descriptor build/relay-yard-artifact/native-gameplay.json `
  --source-directory C:/Assets/KenneyProtagonists `
  --audio-directory examples/relay-yard/audio `
  --output build/relay-yard-contract
```

The graphical verifier additionally needs an installed, stripped Windows runtime
and native Windows Python. It exports the game, removes its owned source copy,
relocates the bundle outside the checkout and runs from an unrelated working
directory. The first process collects one cell and saves; a distinct process
loads that checkpoint from Welcome and completes the objective. Both use the
same native service API as agents. Caller-supplied original sources are retained:

```powershell
python tests/relay_yard_bundle.py `
  --binary build/windows-runtime/poima.exe --runtime build/runtime-installed `
  --artifact build/relay-yard-artifact/native-gameplay.json `
  --source-directory C:/Assets/KenneyProtagonists `
  --audio-directory examples/relay-yard/audio `
  --output build/relay-yard-bundle-check --gpu 1
```

Each run needs a new output folder and retains unsuccessful attempts. The partial
checkpoint occurs before the courier starts travelling. Animation save/load
while moving, the original-source retarget oracle and other subsystem coverage
are separate checks. Native frame readbacks and semantic actions do not prove
physical-device interaction or audible output.

The separate sound verifier records real movement, a native pickup ray and Use,
then reproduces those inputs from a checkpoint with offline capture and continuous
device playback. It saves during the pickup cue, restores its exact cursor and
cadence in fresh processes, and checks that neutral continuation finishes the
cue without duplicating it:

```powershell
python tests/relay_yard_audio.py `
  --binary build/windows-runtime/poima.exe `
  --artifact build/relay-yard-artifact/native-gameplay.json `
  --source-directory C:/Assets/KenneyProtagonists `
  --audio-directory examples/relay-yard/audio `
  --output build/relay-yard-audio-check --gpu 1
```

Use native Windows Python and a real output device for device checks. Linux
headless checks use `--offline-only` with a Linux artifact. Paused graphical
stepping resets the DSP timeline, so the bundle's paused observations do not
substitute for this continuous-play test. Matching mixer statistics and drained
device submission do not prove listening quality, physical loopback, latency or
an uninterrupted waveform across checkpoint restoration. See
[recorded audio evidence](../../docs/evidence/m2-relay-yard-audio.json).
