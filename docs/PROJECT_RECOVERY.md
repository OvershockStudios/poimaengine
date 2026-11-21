# Recover a verified Poima project

With `game.verify.json` beside the manifest, **Export build** captures the
editable project in a checkpoint, exports from that captured source, and runs
its bundled [gameplay verification suite](GAME_VERIFICATION.md). The report
links the exact bundle and suite. A failed export or check leaves the previous
verified build available.

Agents can [build a candidate and retain its passing result](GAME_VERIFICATION.md#run-and-inspect)
from chat. The candidate's source and suite are captured before export. Its
check report is kept in the candidate folder. **Play verified build** and
recovery use only retained passing candidates. A failed or cancelled job is
ineligible for retention.

Choose **Play verified build** to run the last passing export in a separate
window. Poima rechecks the selected checkpoint and launches its bundled
player. You can play a verified build during an agent turn or switch back to
chat while the game runs. The creator can show newer playtest results as
turns finish. The open game keeps the revision it started with.

Playtest saves are kept under
`%LOCALAPPDATA%\Poima\Playtests\<project_id>\<bundle_sha256>`. Close the
standalone game to start a different build or use **Play** to preview the
current working project. If you close Poima first, it waits for the game to
close. When started with standard output redirected, the workspace emits a
`verified_play_closed` event with the build identity, last tick, presented
frame count and stop reason.

### Inspect the verified player

On a creator endpoint, call `desktop.verified.inspect` with empty parameters.
Discover its name in `world.describe` or its request schema in
`desktop.describe`. From MCP, use `poima_call`.

```json
{"method":"desktop.verified.inspect","params":{}}
```

`playback` is null until the first request to play a verified build. It then
includes a `play_id` for this launch, the pinned `bundle_sha256`, `revision`,
and `state`. `checking` means the creator is rechecking the selection.
`running` means its game process launched. A nonzero
`observation.frames_presented` confirms a native presentation. The player
samples `tick`, `frames_presented`, `focused` and `paused` about four times a
second. `observation_age_ms` reports the age of the last published sample.
A save load can replace the runtime and change its tick.

When an earlier packaged runtime supplies only a closure receipt,
`observation` stays null during play. The receipt still supplies final ticks
and frames after the runtime closes. The creator retains the most recent
`closed`, `failed`, or `cancelled` state until the next launch or workspace
close. The working preview keeps its own `player.inspect` contract.

Choose **View playtest results** to see the latest completed attempt and
expand its named groups. Failed checks show the recorded tick and observation.
An interrupted group shows where the playtest stopped. The report identifies
the tested build even when an earlier build is still playable.

Close and reopen the project to find its results and last passing export again. Select
**Restore verified project as copy** to publish an editable project in a
sibling folder, then **Open recovered project** to switch to it. The copy
receives a new project identity and keeps the captured world, assets, source,
input profile and attached native gameplay. Both projects can be edited
independently.

Open **Build history** to browse completed playtests, including earlier
passing versions and failed attempts. Choose a row and use **View checks** to
read its results. **Restore this version as copy** opens only for a retained
passing result. Poima rechecks the selected source snapshot and build before
creating the copy. Use **Older** and **Newer** to move between pages.

## Share a playable ZIP

Choose **Save game ZIP** to save the same build used by **Play verified build**.
Pick a new filename in a local folder outside the editable project and its
history. Sharing rechecks the build's identity and includes only the standalone
game's manifest and files, including the player, assets, gameplay and notices.
Another person can extract the ZIP and open `launch.cmd` on a Windows PC with a
supported Vulkan driver.

The ZIP uses stored members, so its size is close to the exported game folder.
Poima stages the archive beside the destination and publishes the finished
file without replacing an existing one. [ZIP format specification](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)

## Storage and version selection

The creator stores versions beside the project under
`.poima-history/<project_id>/<UTC timestamp>-<suffix>/`. Each version contains
`checkpoint.json` and `source/`. Export adds `game/`, and the verification run
writes `checks.json`. Agent jobs first write `candidate-checks.json`.
In descending directory name order, the native history command returns
`latest_check` for the newest validated completed report and `latest` for the
newest retained passing build. Each can be null. An agent candidate can pass
before retention, so a passing report can have `retained: false`. History
checks the project identity, source and game inventories, suite and report,
and the order, names, ticks and outcomes recorded for the suite. The desktop
checks the report hash again when opening it. Earlier engine versions remain
available when the current engine
can validate the archived source and package formats. The version recorded in
the report must match the archived game and runtime. Play uses that archived
runtime. Keep these folders to retain the versions they contain.

The source inventory includes the local editable files and engine dependencies.
It skips Git metadata, Visual Studio metadata, recognized managed `bin` and `obj`
outputs, the project gameplay adoption receipt journal, and the entry world
and input profile's transient lock, pending and previous files. Attached native
gameplay files are included. External tools, SDKs, linked external source and
player save storage depend on their own installation or backup. The capture
preserves the project's source files and the native artifact it selected at
capture time. Gameplay checks run on that selected artifact.

Source checkpoints share the project's portable path rules and budgets of
4,096 files, 256 MiB per file and 1 GiB of source payload. Keep large external
libraries outside the editable project. Poima uses a new, staged destination
for checkpoint and restore. An existing project or destination is preserved.

## Create worker drafts and stage an integration

Capture the accepted editable project once with `project checkpoint`. Use its
returned `source_sha256` to pin the inventory. Create a draft for each worker
in a new sibling workspace with an existing parent directory.

```powershell
poima project draft .\Base\checkpoint.json --output .\LightingDraft --expected-base-sha256 <source_sha256>
poima project draft .\Base\checkpoint.json --output .\AudioDraft --expected-base-sha256 <source_sha256>
```

Each draft starts with the captured world revision, entity IDs, entry
settings, assets, and gameplay files. It has its own project ID and history.
Open the returned `manifest` in an independent owner using a unique endpoint
name. The source checkpoint can be used before any exported build has passed.

### Check selected scenes in the creator

Save a [playtest suite](GAME_VERIFICATION.md) before adding the first build
worker. The worker round starts from that saved checkpoint. Once workers
finish, choose **Review worker scenes** to read their reports and select which
scene revisions to combine. **Check combined game** builds the selection in a
separate folder and runs the suite captured with the round. Later edits in the
main project stay in that project.

Use **Combined game checks** to inspect the outcome. **Play combined game**
runs the most recent passing combination from this workspace, even after
a later selection fails. **Open combined project as copy** opens an editable
copy of the verified source in a new creator window. It gets its own project
identity. The original project's build history and chat stay with the original
workspace. Combined checks, exports and worker receipts are stored alongside
the drafts under `.poima-drafts`. Reopen the original project to restore that
round's worker tabs, most recent checks and last passing build. If an unfinished
task was interrupted, inspect its draft before sending a follow-up. A report
whose scene changed after completion needs a new review before combining.

Choose **Start new worker round** when the current workers are idle to use a
new checkpoint for later assignments. The previous draft and build files stay
in their round folder. Copied and relocated projects have their own round
selection. Drafts made before Poima recorded worker rounds remain available
through their saved manifests.

The creator combines scene changes and their cooked dependencies. Use the CLI
when the integration also needs selected source files.

### Select scene and source changes by hash

The lead reviews up to ten drafts and uses each draft's `world_sha256` from
`project inspect` to select its scene. Set `files` to the other files to carry
over, with their reviewed hashes and the same relative paths in the candidate.
Poima selects the cooked assets and provenance needed by the pinned scenes.

```json
{
  "base_sha256": "<source_sha256>",
  "drafts": [
    {"manifest": "D:/Work/AudioDraft/project.poimaproject", "world_sha256": "<audio world hash>", "files": []},
    {"manifest": "D:/Work/LightingDraft/project.poimaproject", "world_sha256": "<lighting world hash>", "files": []}
  ]
}
```

Save the request as `changes.json`.

```powershell
poima project integrate .\Base\checkpoint.json --output .\Integrated --request .\changes.json
```

Poima compares each pinned scene with the common checkpoint. It can combine
a rename and a lighting component change on the same entity. Divergent edits
to a name, parent, or whole component are reported at that field. Additions
and deletions use the complete entity as their unit. UI elements, templates,
registered schemas and provenance use whole entries. Navigation uses one
binding. The merged world keeps retired identities, and identity reuse,
invalid hierarchies and broken asset references fail validation.

Pinned scene selection requires the checkpoint's project settings and entry.
The result lists the selected world hashes and the candidate world hash. A
merged scene advances the checkpoint revision by one and carries forward its
existing receipts. Worker receipts stay in the original drafts.

For a manually curated scene edit, use one ordinary `world.transact` against
the captured revision and omit the draft world hashes. In this mode the lead
also selects each cooked asset referenced by the transaction.

```json
{
  "base_sha256": "<source_sha256>",
  "drafts": [
    {"manifest": "D:/Work/AudioDraft/project.poimaproject", "files": [
      {"path": "audio/ambience.wav", "sha256": "<reviewed file SHA-256>"}
    ]},
    {"manifest": "D:/Work/LightingDraft/project.poimaproject", "files": []}
  ],
  "transaction": {
    "request_id": "1234567890abcdef1234567890abcdef",
    "base_revision": 2,
    "ops": [{"op": "entity.rename", "id": "00000000000000000000000000000005", "name": "Window light"}]
  }
}
```

The native command validates hashes, project lineage, the assembled scene,
and the complete dependency closure before publishing a separate candidate.
Identical file contributions can be shared. Conflicts at one path fail.
Integration accepts up to 4,096 selected files, 1 GiB of selected data, and
a 1 MiB request. The candidate also fits the source checkpoint's limits.

Use the returned candidate manifest to playtest or export the joined work. The
candidate has its own project ID. The creator can open it separately. The
checkpoint, draft inputs, and the live project used to make the checkpoint
stay available for comparison.

## CLI workflow

Supply a new checkpoint directory in an existing parent outside the source.
Use the returned `source_manifest` for export. The archive expects the game
at `game/game.json` and the completed report at `checks.json`.

```powershell
poima project checkpoint .\MyGame\project.poimaproject --output .\History\2026-11-20-a
poima project build .\History\2026-11-20-a\source\project.poimaproject --output .\History\2026-11-20-a\game --runtime .\Runtime
.\History\2026-11-20-a\game\runtime\bin\poima.exe game verify .\History\2026-11-20-a\game\game.json --suite .\History\2026-11-20-a\source\game.verify.json --report .\History\2026-11-20-a\checks.json
poima project history .\History --project-id 0123456789abcdef0123456789abcdef
poima project versions .\History --project-id 0123456789abcdef0123456789abcdef
poima project recover .\History\2026-11-20-a\checkpoint.json --output .\MyRecoveredGame
poima project share .\History\2026-11-20-a\checkpoint.json --expected-bundle-sha256 <bundle-sha256> --output .\MyGame.zip
```

Use the project's real ID from `project inspect`. `project history` takes the
history directory directly, so it also works when the original project's path
is unavailable. To recover a specific older passing version, supply its
`checkpoint.json`. Recovery checks the archived source and build before
publishing the copy. Use the returned `restored_manifest` to edit or export it.
For sharing, take the 64-character `bundle_sha256` from the retained passing
result. The command reports the archive path, byte count, file count and exact
build identity.

`project versions` checks at most six history directories per page and
returns valid entries in descending directory order, with an unavailable
count in `skipped`. Pass a non-null `next_before` as `--before` to inspect
older directories. Each returned `snapshot_sha256` covers the checkpoint
manifest and its verification report, which pin the source inventory and
the tested bundle. Pass it to `project recover` as
`--expected-snapshot-sha256` to restore the selected snapshot.

Run `python tests/project_recovery_cli.py --runtime <installed-runtime> --output <new-directory>`
to reproduce the native checkpoint, validation and recovery checks on a matching
host. The installed runtime must include simulation and the native player.
On Windows, run `tests/creator_workspace.ps1 -Editor <packaged-editor-exe> -Output <new-directory> -WithPlaytestResults`
to exercise the results dialog, restart, verified play and recovery.
Use `-WithShareZip` to exercise saving and cancelling through the Windows dialog,
then extract and launch the shared game.
Use `-WithBuildHistory` to verify that the creator can inspect a failed check
and restore the older of two passing revisions.
