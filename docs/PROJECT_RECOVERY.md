# Recover a verified Poima project

With `game.verify.json` beside the manifest, **Export build** captures the
editable project in a checkpoint, exports from that captured source, and runs
its bundled [gameplay verification suite](GAME_VERIFICATION.md). The report
links the exact bundle and suite. A failed export or check leaves the previous
verified build available.

Close and reopen the original project to see its last verified build. Choose
**Open build folder** to play the exported game. Select **Restore verified
project as copy** to publish a new editable project in a sibling folder, then
**Open recovered project** to switch to it. The recovered copy receives a new
project identity and keeps the captured world, assets, source, input profile
and attached native gameplay. Both versions can be edited independently.

## Storage and version selection

The creator stores versions beside the project under
`.poima-history/<project_id>/<UTC timestamp>-<suffix>/`. Each version contains
`checkpoint.json` and `source/`. Export adds `game/`, and the verification run
writes `checks.json`. The native history command selects the newest complete
passing version in descending directory name order. It checks project identity,
source inventory and hashes, the suite and report, and the full game inventory.
It can fall back to an earlier passing version when a newer version fails or is
incomplete. Keep these folders to retain the versions they contain.

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
poima project recover .\History\2026-11-20-a\checkpoint.json --output .\MyRecoveredGame
```

Use the project's real ID from `project inspect`. `project history` takes the
history directory directly, so it also works when the original project's path
is unavailable. To recover a specific older passing version, supply its
`checkpoint.json`. Recovery checks the archived source and build before
publishing the copy. Use the returned `restored_manifest` to edit or export it.

Run `python tests/project_recovery_cli.py --runtime <installed-runtime> --output <new-directory>`
to reproduce the native checkpoint, validation and recovery checks on a matching
host. The installed runtime must include simulation and the native player.
