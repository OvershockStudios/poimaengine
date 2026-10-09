# Compiled player preferences menu fixture

`Poima.Tests.ManagedPlayerPreferencesGame` is a stateless, genuinely compiled
`Game<PlayerPreferencesMenuState>` consumer of the independent
`IPlayerPreferencesGame` opt-in. It needs epoch 7 services with a 256-byte readable
prefix and the named `player_preferences_v1` feature. It gains no navigation,
animation, character-input or instance extension through that larger prefix.

The native scene supplies stable UI IDs: proof panel 500, proof status label 501
and proof button 502 (`live`), plus a visible menu opener 699, hidden modal menu
panel 700 and full menu status label 701. Menu buttons 710 through 724 dispatch
the `pref.*` tokens in `Control`. Opening makes the menu panel modal
and requests pause; closing clears it and requests resume. Button actions tune
FOV, mouse sensitivity, inversion, UI scale and master gain, remove selected/all
overrides, or request next-launch graphics. There are no sliders, text inputs,
automatic profile writes or invented widgets.

Every callback reads through `GetPlayerPreferences`. Every mutation calls
`TryStagePlayerPreferences`, uses the native owner/revision guard, queries the
staged ticket, and asserts that another read still sees committed configuration.
The displayed status separates staging, committed configuration revision and
observed/applied/presented device revisions. Diagnostic state records the queried
ticket's immutable accepted revision separately. Refresh or reopening updates a
paused menu; the fixture claims no paused animation. Current graphics remain
frozen while the snapshot reports requested next-launch graphics separately.
Inherited FOV/UI may be unavailable until observed, and actual window density
can exceed the bounds allowed for a configured UI override.

`Mode=0` only observes and queries during Tick. Mode 1 attempts one patch per
Tick, exposing native Busy on a second Tick in one step boundary. Mode 2 stages
on an even Tick then throws on the following odd Tick. Mode 3 stages then assigns
an invalid live entity reference for native validation. Mode 4 throws immediately
after staging. These are real callbacks; the runner may set the test mode but
must activate native UI buttons to exercise Control actions.

Diagnostic fields record observations, guards and bounded process-local tickets;
they never publish preferences or restore preferences from gameplay saves.
`pref.stage.throw` and `pref.stage.invalid` exercise Control rollback;
`pref.remember`/`pref.stale` exercise guarded conflicts; `pref.query` inspects the
retained ticket after acceptance or owner replacement. Save actions use the
existing gameplay-save service and slot `player-preferences` (save generation 0,
load generation 1). `pref.stage.save` and `pref.stage.load` combine staging with
those native requests to exercise publication before save/load replacement.

The [0.0.81 recorded cohort](../../docs/evidence/m2-compiled-player-settings.json)
checks Linux headless CoreCLR/Native AOT and Windows attached callbacks on both
laptop GPUs with both backends. The menu is rendered and inspected; native
presentation requires the native window path. Physical input, listening and
source-free exported-menu playback remain separate checks.

To run the menu, build this project together with the matching managed bridge,
then supply its assembly, bridge and managed host paths to
`tests/player_preferences_gameplay_contract.py --config <configuration.json>`.
The default headless runner authors the native scene and loads this exact game
type to check unavailable callbacks. Add `--capture` with the actual Windows
native player prerequisites to attach a player, open the visible menu and
activate its actual controls. The matching genuine NativeAOT artifact can be
supplied through the runner's native descriptor configuration instead. See the
runner's `--help` for required engine, backend and output arguments; execution
evidence belongs to its output directory.

Follow [the compiled player settings guide](../../docs/COMPILED_PLAYER_SETTINGS.md)
for exact API/registry contracts, complete build and runner commands, native
ordering, save isolation and recovery.

The native scene has a compact button menu:

| UI ID | Action | Meaning |
| --- | --- | --- |
| 699 | `pref.open` | Open modal settings and pause |
| 710 / 711 | `pref.fov.minus` / `pref.fov.plus` | FOV −/+ 5 degrees |
| 712 / 713 | `pref.sensitivity.minus` / `pref.sensitivity.plus` | Mouse X/Y −/+ .05 |
| 714 | `pref.invert` | Toggle vertical mouse inversion |
| 715 / 716 | `pref.ui.minus` / `pref.ui.plus` | UI scale −/+ .25 |
| 717 / 718 | `pref.gain.minus` / `pref.gain.plus` | Master gain −/+ .1 |
| 719 / 720 | `pref.reset` / `pref.reset.all` | Reset FOV/UI/gain or all sparse overrides |
| 721 / 722 | `pref.graphics.low` / `pref.graphics.high` | Next launch 1/1 or 4/2 samples/frames |
| 723 | `pref.refresh` | Read fresh committed/application status |
| 724 | `pref.close` | Close modal settings and resume |

The menu status reports sources, input tuning, frozen versus requested graphics,
and cached application observations. Staging is not acceptance or presentation;
refresh shows subsequent status. Failed or stale requests remain readable and
require deliberate refresh. All nine values use the native registry's exact
bounds and inheritance rules; graphics dependencies remain native validation.
