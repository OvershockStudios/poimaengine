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
player. Playtest saves are kept under
`%LOCALAPPDATA%\Poima\Playtests\<project_id>\<bundle_sha256>`. Close the game
window to return to editing. **Play** previews the current working project.

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
