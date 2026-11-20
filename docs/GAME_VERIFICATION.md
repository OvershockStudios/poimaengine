# Verify gameplay in an exported game

A verification suite describes fixed ticks, semantic player inputs and expected
native observations. Run it with the executable inside a bundle exported using
Poima 0.0.287 or later.
The report records SHA-256 for the exact game manifest and suite. The manifest
covers the world, cooked assets, input profile, compiled gameplay and packaged
runtime. The verifier checks that inventory before and after the test.

## Write a suite

Use `poima.game-verification` version 1. A group starts in its own fresh game
session with the bundle's normal initial gameplay values. Groups may share the
same conditions or exercise different paths from the start. Each group needs
at least one expectation. Name groups and expectations so the outcome can be
read in the workspace. Replace this sample controller ID with the controller
from `game inspect` when using a different project.

```json
{
  "format": "poima.game-verification",
  "version": 1,
  "groups": [
    {
      "name": "The player can walk forward",
      "sequence": [
        {"step": {"ticks": 15, "inputs": [{"entity": "00000000000000000000000000000002", "move": [0, 1]}]}},
        {"expect": {
          "label": "The starter player moved toward the far end of the room",
          "method": "runtime.entity",
          "params": {"id": "00000000000000000000000000000002"},
          "path": "/world_matrix/14",
          "between": [0.5, 1.8]
        }}
      ]
    }
  ]
}
```

`step` accepts 1 to 600 ticks and optional native [semantic input
records](RUNTIME.md#try-the-physics-room). `expect` accepts `runtime.inspect`,
`runtime.entity`, `runtime.gameplay.inspect`, `runtime.ui.inspect`,
`runtime.component.get`, or `runtime.raycast`. Pass the method's ordinary
parameters and let the verifier supply the current session and tick. `path`
is a JSON Pointer into the result. Use `equals` for JSON value equality or
`between` for an inclusive numeric interval. A missing path fails even when
`equals` is `null`.

The suite is limited to 1 MiB, 32 named groups, 512 operations, 36,000 total
ticks and a nesting depth of 32. Each `step` accepts up to 32 input records.
The verifier uses headless simulation with saving disabled. Use the [game
session service](GAME_SERVICE.md) for save continuation, interactive
presentation and checks beyond the suite format.

## Run and inspect

```powershell
.\Export\runtime\bin\poima.exe game verify .\Export\game.json --suite .\game.verify.json --report .\export.checks.json
```

Supply an existing exported bundle and suite. An optional report needs a new
file outside the bundle in an existing directory. Exit code 0 means every
group passed. Exit code 4 means a check or operation failed. A completed suite
returns group counts, simulated ticks, individual observations and issues.
Missing or invalid setup returns diagnostics. An assertion failure leaves the
other groups available to run. Review `actual` and `observed` at the check's
recorded tick. Very large observations are omitted from the report.

In the Windows creator, save the suite as `game.verify.json` beside the project
manifest. **Export build** runs it against the exported candidate and exposes
**Open check report**. A failed check leaves an earlier export available in the
current workspace. Output directories and their reports remain on disk. Source
project restoration and workspace history across restarts have separate gates.

Use tests that express the agreed game brief. Coverage is visible in the named
checks and their exact input hash. Rendered appearance, audible output and
physical controls require their own observations.

## Reproduce the CLI contract

Run `python tests/game_verification_cli.py --runtime <installed-runtime> --output <new-directory>`
on a matching host with a renderer-enabled runtime distribution. The test creates and exports the
starter game, relocates the bundle, executes passing and failing suites, and
checks that reports and verification preserve the bundle's exact bytes.
