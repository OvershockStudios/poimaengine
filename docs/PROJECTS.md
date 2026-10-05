# Projects and native game bundles

Poima 0.0.24 adds a project manifest, a first-person starter, read-only project inspection, and export into a self-contained content directory with an installed native Poima runtime. The same compiled API serves the CLI. This is the first distribution path for the existing native simulation/player; a package manager and a finished production shipping pipeline remain future work. Version 0.0.35 adds [native compiled C# gameplay](NATIVE_GAMEPLAY.md) through project/game manifest v2.

[Checkpoint evidence](evidence/m2-projects-editor-docking.json) records 11 project/export checks on both Linux and Windows, 24 headless and 28 simulation checks, and relocated native replay on both laptop GPUs. The relocated replay reproduces source pixels and final state with unchanged bundle contents; platform and deployment limits below still apply.

## Create and inspect

The parent directory must already exist. Creation requires a new directory, including when the requested destination is an existing empty directory.

```sh
./build/headless/poima project create build/MyRoom --name "My Room"
./build/headless/poima project inspect build/MyRoom/project.json
```

The starter contains `project.json`, `world.json`, and `settings/default.poima-input.json`. Its world has a solid floor, a capsule character, a child camera, and an Environment with a procedural sky and shadow-casting directional Sun. Sky defaults apply to new projects; existing worlds are unchanged. The persisted v2 input profile supplies keyboard/mouse and gamepad defaults. Creation removes its own writer-lock sidecars before publication.

Project inspection validates the world and every referenced cooked asset without creating writer sidecars or repairing files. It reports the project identity, entry entities, authored revision, dependencies, audio requirement and optional input profile. Missing worlds are errors; inspection never creates a blank replacement.

A project manifest has this shape; actual generated identities differ:

```json
{
  "format": "poima.project",
  "version": 1,
  "project_id": "0123456789abcdef0123456789abcdef",
  "name": "My Room",
  "entry": {
    "world": "world.json",
    "controller": "00000000000000000000000000000002",
    "camera": "00000000000000000000000000000003"
  },
  "input_profile": "settings/default.poima-input.json",
  "audio": false
}
```

`input_profile` is optional; `audio` is optional and defaults to `false`. Unknown fields, duplicate JSON keys, unsupported versions and incorrect types are rejected. Version 1 has no gameplay field. Version 2 requires `gameplay: {"descriptor":"gameplay/native-gameplay.json","values":{}}`, where `values` is optional. The descriptor names an inventoried native compiled game artifact; development IL and hostfxr are not shipped by this route. See [native gameplay](NATIVE_GAMEPLAY.md) for schema, target and module lifetime rules.

Paths resolve relative to the manifest's directory, independently of the process working directory. Relative content paths use `/` separators and ASCII letters, digits, spaces, `_`, `-` and `.`. Absolute paths, backslashes, traversal components, Windows device names, trailing dots/spaces, symlinks and content-path junctions are rejected. Display names and the containing project directory can use UTF-8. Relative paths are bounded to 1,024 bytes and components to 128 bytes; project manifests are bounded to 64 KiB.

The entry IDs must identify a `CharacterController` and `Camera`. The controller must be an unscaled hierarchy root with only Y-axis rotation. Its configured camera must be the selected camera and a direct child. World validation also checks the existing component and reference rules.

Edit the source world using the [world service](WORLD_SERVICE.md), [shared sessions](SHARED_SESSIONS.md), or the existing native authoring API. Project inspection reads the committed source files; it does not publish unapplied Inspector drafts.

## Install a runtime and export

Export takes an installed runtime tree, not a raw build directory. Build a renderer-and-simulation configuration and install it into a dedicated prefix. For the existing Windows cross-build route:

```sh
cmake --build build/windows-runtime --target poima
cmake --install build/windows-runtime --prefix build/runtime-windows

./build/headless/poima project build build/MyRoom/project.json \
  --output build/MyRoom-game --runtime build/runtime-windows
./build/headless/poima game inspect build/MyRoom-game/game.json
```

The exporter accepts only `runtime.json`, `bin/`, `lib/` and `share/poima/` in its runtime root. The default install includes only runtime files, including when the desktop bridge is enabled. Its C ABI development header is opt-in: `cmake --install build/windows-runtime --component Development --prefix build/desktop-sdk`. Keep that SDK prefix separate from the runtime. Use a fresh runtime prefix if an older installation left development headers in it; CMake does not remove obsolete installed files.

The exporting headless executable and installed runtime must have the same exact engine version. The runtime descriptor, generated and installed by CMake, is `runtime.json`:

```json
{
  "format": "poima.runtime",
  "version": 1,
  "engine_version": "0.0.35",
  "target_os": "Windows",
  "target_arch": "x86_64",
  "executable": "bin/poima.exe",
  "features": {
    "simulation": true,
    "renderer": true,
    "audio": false,
    "managed": false,
    "native_gameplay": true,
    "editor": false
  }
}
```

Actual feature values come from that build. A Linux descriptor uses `target_os: "Linux"` and `bin/poima`. Both targets currently require `x86_64`. Simulation and renderer must be enabled; gameplay projects additionally require `native_gameplay: true` (an absent feature defaults to false); an audio-enabled runtime is required when the world dependency inspection or project audio flag requires it. On Linux, the runtime executable must retain owner-execute permission. Windows-to-Linux export is rejected because executable-mode preservation has not been qualified.

The installed tree includes the executable and dependency notices under `share/poima/`. Audio builds also install `bin/phonon.dll` on Windows or `lib/libphonon.so` on Linux. SDL, NVRHI, Jolt and the Windows compiler runtime are statically linked in the existing configuration. The graphics shaders are embedded; the bundle does not need shader sources or DXC. Windows still needs its supported OS/UCRT components and system Vulkan loader/driver. These system dependencies are not copied out of the development machine.

The current Steam Audio path uses CPU acoustics. `phonon.dll` has delayed imports for optional accelerated audio components; copying this CPU runtime does not qualify those optional GPU audio paths.

A resulting bundle is organized as follows:

```text
MyRoom-game/
  game.json
  launch.cmd                         # launch.sh for Linux
  content/
    world.json
    world.json.assets/               # present when dependencies exist
      <sha256>.pmodel
      <sha256>.pimage
      <sha256>.paudio
    default.poima-input.json         # when configured
  runtime/
    runtime.json
    bin/poima.exe                    # bin/poima for Linux
    bin/phonon.dll                   # optional; Linux uses lib/libphonon.so
    share/poima/...
```

The content closure includes all referenced assets, including invisible meshes and disabled emitters. `StaticMesh`, `SkinnedMesh` and `AnimationRig` reference cooked models. A material texture map references a cooked model when it selects an embedded image, otherwise a cooked image. Audio emitters reference cooked audio. Models already embed their geometry, material images, skins and animation curves; source glTF files and their original external textures are not copied. Unreferenced cooked assets, lock/staging files and previous-world backups are excluded. The bundled world removes authoring receipts and retired-ID bookkeeping; its original identity, revision and entities remain.

`game.json` records the project identity, entry IDs, engine target, audio/input settings, source world revision/hash, and a sorted inventory of payload paths, sizes, SHA-256 hashes and roles. Its source hash describes the original authored world bytes; the inventory separately hashes the sanitized bundled world. The manifest excludes its own hash. Bundle identity contains no export timestamp or absolute source path. Hashes detect changes relative to the manifest; they are not publisher signatures.

Budgets are 4,096 payload files, 256 MiB per file and 1 GiB total payload, alongside existing world/model/image/audio limits. The game manifest is limited to 4 MiB. Export validates and rechecks source content, copies into owned staging, verifies the staged bundle, and refuses destinations overlapping the source project or runtime tree.

## Publication and immutability

Existing output directories are never replaced. Successful creation/export reports `publication`:

- `atomic_directory`: the platform publishes the prepared directory with no-replace rename semantics.
- `manifest_last`: used on Linux filesystems such as WSL DrvFS that reject `renameat2(RENAME_NOREPLACE)`. Poima reserves a new destination with `mkdir`, creates payloads exclusively, writes the manifest last, and removes an incomplete marker only after completion.

The fallback exposes a directory while it is being populated; project/game readers reject it while `.poima-incomplete` exists. If publication fails or is interrupted, a partial **new** destination can remain. It is not reused automatically: inspect/remove it explicitly or retry into a different destination. No fallback uses an ordinary replacing rename to bypass an existing directory.

`game inspect` and `game run` verify the inventory and dependency closure. Missing, modified, extra or incorrectly classified files are errors. Put captures, reports, replay experiments and other added files outside the bundle. To change shipped content, edit the source project and build a fresh destination.

## Run or replay

On Windows, double-click the bundle's `launch.cmd`, or run its native executable directly:

```text
MyRoom-game\runtime\bin\poima.exe game run MyRoom-game\game.json
```

On Linux use `./MyRoom-game/launch.sh`. The convenience launchers locate their own bundle; direct CLI paths follow the ordinary process working directory. `game inspect` can inspect a foreign-target bundle, and a Linux host can export an installed Windows runtime without executing it. `game run` requires the executable's OS and architecture to match the bundle and requires its simulation/Vulkan player capabilities.

The player reads the bundled world and profile without writer sidecars. Runtime state stays in memory; running does not persist edits into the bundle. The existing player supports keyboard/mouse, the configured gamepad profile and window resizing. Public game-save slots and persistence remain separate future work.

| Option | Behavior |
| --- | --- |
| `--gpu N` | Vulkan device index, 0–4095; omit for automatic selection. |
| `--frames N` | Interactive frame limit, 1–36000; omit to run until the player closes. |
| `--replay PATH` | JSON array of semantic input segments; mutually exclusive with `--frames`. |
| `--capture PATH` | Final BMP at an external, previously absent path. |
| `--report PATH` | Native result JSON at an external, previously absent path. |
| `--width N`, `--height N` | Each 128–4096; defaults 960×540. |
| `--samples N` | 1 or 4; default 4. |

Capture/report parent directories must exist. Their destinations must differ, be outside the bundle and not overlap the replay input. Existing destinations are rejected. The report file contains the result object, including the player report, final runtime state and entry entity states; stdout contains the normal structured reply envelope.

A replay file can contain:

```json
[
  {"ticks": 120},
  {"ticks": 60, "move": [0, 1]},
  {"ticks": 1, "look": [20, 0], "jump": true}
]
```

Replay files are bounded to 1 MiB and 1–256 segments. Each segment uses the [player replay contract](PLAYER.md), including permitted motion and sound commands; total replay length is bounded. Replays use semantic input rather than physical keybindings. The project's `audio` flag controls player device output; there is no command-line audio override in this checkpoint.

## Native API and present limits

[`poima/project.hpp`](../include/poima/project.hpp) exposes `create_project`, `inspect_project`, `build_project`, `inspect_game` and `load_game`. The first four return structured `Reply` values; `load_game` verifies the bundle and returns a `GameDefinition` with resolved paths, or throws. [`poima/game_launch.hpp`](../include/poima/game_launch.hpp) exposes `run_game`. Discover CLI argument shapes using `poima schema project` and `poima schema game`.

Exact engine-version equality is deliberate in this first format. There is no bundle migration, package resolver, incremental patcher, signing or installer integration. Export copies an existing native runtime; it does not compile game code. Version 2 packages an already published native C# gameplay artifact and validates its target, schema, initial values and inventory. CoreCLR development assemblies and hostfxr are not copied by this route. [Native gameplay](NATIVE_GAMEPLAY.md) describes its process lifetime and qualification limits; [CoreCLR authoring](MANAGED_GAMEPLAY.md) remains the reloadable development path.

A relative Linux library layout does not establish broad Linux binary compatibility. CMake installation rewrites the audio runtime's build-tree search path to `$ORIGIN/../lib`; the raw build executable should not be distributed in its place. One inspected development build required GLIBC 2.38 and GLIBCXX 3.4.32. A supported distribution baseline and clean-machine Linux graphics launch remain qualification work.

Tests are supplied in `tests/project_contract.py` and `tests/game_bundle_capture.py`. They target source preservation, malformed manifests, dependency inventories, relocation, clean working-directory/environment launch, exact replay images and final state. The linked checkpoint evidence records their successful runs and remaining qualification limits.

Version 0.0.35 additionally uses `tests/project_gameplay_contract.py`, `scripts/verify_native_gameplay.py`, `scripts/verify_native_gameplay_parity.py` and `scripts/verify_native_game_bundle.py` to check actual compiled game modules, native/development correspondence, v2 inventories and relocated Vulkan game launch. [Native gameplay evidence](evidence/m2-native-gameplay.json).
