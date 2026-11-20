# Verify gameplay in an exported game

A verification suite describes fixed ticks, semantic player inputs and expected
native observations. Run it with the executable inside a bundle exported using
Poima 0.0.287 or later.
The report records SHA-256 for the exact game manifest and suite. The manifest
covers the world, cooked assets, input profile, compiled gameplay and packaged
runtime. The verifier checks that inventory before and after the test.

## Write a suite

An agent connected to a host with a configured development profile can read
the project's `game.verify.json` with `project.verification.read` through
`poima_call`. Pass `profile` and the profile-relative `project` manifest.
To create or replace the suite, call `project.verification.write` through
`poima_checks` with the same selectors, the complete suite JSON in `text`
and the returned `sha256` as `expected_sha256`. Use a null guard if the file
is absent. The service validates the suite format before publishing it.
Read the file again if a concurrent edit changes its hash. In the creator,
Codex's **Allow gameplay check edits in this session** authorizes these
writes. Run **Export build** to execute the saved suite.

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

An agent can build a recoverable candidate from the saved project through the
connected creator workflow. Call `development.candidate` through `poima_build`
with `profile`, `project`, a fresh 32-hex `request_id` and the
`expected_suite_sha256` from `project.verification.read`. The worker captures
the source and suite, builds that snapshot and runs the tests. Inspect the
`job_id` with `development.inspect`, `include_output: false`. Once a completed
result has `passed: true`, call `project.candidate.retain` through `poima_build`
with the same profile, project and candidate `job_id`. The history then holds
the exact playable build and its editable source. The creator refreshes that
history after the turn. [Project recovery](PROJECT_RECOVERY.md)

For an export or check run without a source checkpoint, use separate jobs.
The selected profile supplies the installed runtime and output directory.

1. Read `project.verification.read` and keep the suite's `sha256`.
2. Submit `development.export` through `poima_build` with `profile`, `project`,
   a new `output` directory, and a fresh 32-hex `request_id`. Poll the receipt's
   `job_id` with `development.inspect` until the export succeeds.
3. Submit `development.verify` through `poima_build` with the same `profile`
   and `project`, the `export_job_id`, the `expected_suite_sha256`, a new
   `request_id` and an `output` filename such as `first-checks.json`. The
   report lives beside the bundle under the profile's output root.
4. Inspect the verification job with `include_output: false`. A completed
   verdict has `state: "succeeded"`. Its `result.passed` indicates whether
   the gameplay passed. The summary includes the bundle and suite hashes,
   check counts, up to 16 failures or execution issues, and the full report
   path and hash. A failed or cancelled job publishes no verdict.

Use **Allow gameplay builds in this session** to authorize these operations
for Codex. The creator's **Export build** action also records a recoverable
checkpoint with the saved suite and editable project source.

For direct CLI use, run the executable from the exported bundle.

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
Pass `--expected-bundle-sha256` and `--expected-suite-sha256` to pin the exact
inputs. `--completed-exit true` requires `--report` and exits zero once it
writes a complete verdict, including a failed suite. Read `result.passed`
when using that mode.

In the Windows creator, save the suite as `game.verify.json` beside the project
manifest. **Export build** runs it against the exported candidate and exposes
**Open check report**. A failed check leaves an earlier export available in the
workspace. The [project checkpoint](PROJECT_RECOVERY.md) keeps the verified
bundle, report and editable source available across restarts.

Use tests that express the agreed game brief. Coverage is visible in the named
checks and their exact input hash. Rendered appearance, audible output and
physical controls require their own observations.

## Reproduce the CLI contract

Run `python tests/game_verification_cli.py --runtime <installed-runtime> --output <new-directory>`
on a matching host with a renderer-enabled runtime distribution. The test creates and exports the
starter game, relocates the bundle, executes passing and failing suites, and
checks that reports and verification preserve the bundle's exact bytes.

Run `python tests/development_verification_cli.py --runtime <installed-runtime> --output <new-directory>`
to exercise the owned jobs. It checks passing and failing verdicts, guarded
retries and refusal to start a modified bundled executable.
