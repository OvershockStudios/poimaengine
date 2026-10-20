# Adopt compiled gameplay into a project

`project.gameplay.adopt` attaches a verified native gameplay artifact to a
project. It copies the artifact into a new project-owned directory and updates
the project manifest. It does not execute the module, edit the world, activate
live gameplay, compile code or upgrade saves.

Use this after [publishing native gameplay](DEVELOPMENT_PROFILES.md), before
exporting the project. [Project inspection](PROJECTS.md) supplies the exact
`manifest_sha256`. Compute SHA-256 from the exact artifact descriptor file, or
use its verified publication receipt hash.

## CLI

Write a request file with a fresh 32-character lowercase hexadecimal ID and
the SHA-256 hashes of the current manifest and selected artifact descriptor:

```json
{
  "request_id": "0123456789abcdef0123456789abcdef",
  "expected_project_sha256": "<64 lowercase hexadecimal characters>",
  "expected_artifact_sha256": "<64 lowercase hexadecimal characters>",
  "values": {}
}
```

```sh
poima project inspect MyGame/project.json
poima project adopt-gameplay MyGame/project.json \
  --artifact products/native-gameplay.json --request adopt-request.json
poima project build MyGame/project.json --output MyGame-player --runtime runtime
```

Omitting `values` preserves the current version-2 project's initial gameplay
values, or uses an empty object for a version-1 project. Explicit `{}` resets
those initial values. Values must satisfy the selected gameplay schema.

Success promotes the manifest to version 2 and selects
`gameplay/adopt-<request_id>/native-gameplay.json`. Earlier adopted artifacts
remain intact. The copied descriptor, library, dependencies and notices must
match the verified inventory. The artifact must match the native host's OS,
architecture and available gameplay contract. Its recorded engine version is
diagnostic; compatibility depends on the actual ABI and required features.

## World service and MCP

An owner-configured development profile defines the project and product roots.
These methods accept relative selectors within those roots:

```json
{"method":"project.inspect","params":{"profile":"native","project":"MyGame/project.json"}}
```

```json
{
  "method": "project.gameplay.adopt",
  "params": {
    "profile": "native",
    "project": "MyGame/project.json",
    "artifact": "products/native-gameplay.json",
    "request_id": "0123456789abcdef0123456789abcdef",
    "expected_project_sha256": "<64 lowercase hexadecimal characters>",
    "expected_artifact_sha256": "<64 lowercase hexadecimal characters>",
    "values": {}
  }
}
```

MCP exposes inspection through `poima_call` and adoption through the separately
authorized `poima_build` tool. Adoption is synchronous and creates no development
job. It requires the profile's native adoption callback. These operations are
absent from read-only packaged-world discovery and are outside the stable
authoring-core v1 contract.

## Retry and recovery

Keep the complete request and artifact selector when retrying. Reusing an ID
with different hashes, values or selectors is a conflict. A durable project
receipt allows the same request to survive a host restart.
Preserve field presence too: omitted `values` and explicit `{}` are different
requests, even when they resolve to the same initial values.

A successful historical retry verifies the retained copy and the current
project, then returns `replayed: true` without rewriting the manifest. It also
returns `current_manifest_sha256` and `attached`. The latter means the current
manifest selects that descriptor; it does not assert equality of current
initial values. Later edits and later adoptions are preserved. The original
source artifact may be absent once a complete verified copy is retained.

An interrupted preparation can resume only at its recorded before/after
manifest boundary, with unchanged world and input-profile hashes. Missing
payload files require the unchanged original artifact. Modified final copied
files are rejected rather than replaced. A complete, validated terminal staging
record can finish receipt publication even after later project edits.

Payloads are flushed and published with atomic no-replacement moves.
Windows adoption file operations use extended paths for long staging names;
stored selectors and returned paths keep their ordinary spelling. A platform
or filesystem without those semantics fails explicitly. On Linux, publication
requires `renameat2(RENAME_NOREPLACE)`. A mounted Windows filesystem in WSL
rejects that operation in the [Locomotion Yard qualification](evidence/m2-locomotion-rates.json);
use a native Linux project filesystem for that workflow. The same worker passes
there without substituting an overwriting rename or modifying its requirements.
The manifest changes
through an atomic replacement after validation. Request-owned torn staging is
retained in bounded diagnostics before reconstruction; invalid initial journal
staging without a canonical prepared record is rejected. These recovery rules
are not a guarantee against storage-device failure or loss of unsynced
filesystem metadata.

The project owns `.poima-gameplay-adoptions.lock`,
`.poima-gameplay-adoptions/`, `.poima-gameplay-adoption-staging/` and temporary
`.poima-adopt-<request_id>.pending` manifest files. Preserve them with the
project when recovery or retry receipts are needed. An older interrupted
request does not block a new request guarded by the current manifest hash;
retrying that older preparation after the manifest moves on is a conflict.

## Bounds and errors

Requests and manifests are limited to 64 KiB. Each selected journal is limited
to 1 MiB, with at most 128 retained request IDs and 256 journal-directory
entries. Artifact closure is limited to 257 files including its descriptor,
256 MiB per payload and 1 GiB of payload plus its 1 MiB descriptor. Each request can retain at most eight torn
stages totalling 1 GiB. Artifact, manifest and receipt paths reject links, reparse points, hard-linked files,
case collisions and traversal.

| Error code | Meaning |
| --- | --- |
| `-32602` | Invalid request or explicitly supplied typed values. |
| `-32009` | Stale guard, changed retry, incompatible preparation boundary or unavailable writer lock. |
| `-32091` | Invalid artifact, retained-file corruption, journal corruption or filesystem failure. |

Native adoption failures carry `error_code` in the diagnostic and exit with status 4.
CLI argument or request-file usage errors retain the ordinary usage-error status.
Service failures preserve that code. Project inspection is read-only and never
repairs a failed adoption.

## Qualification and reproduction

[Windows results](evidence/m2-project-gameplay-adoption.json) record the native
adoption, recovery and compiled-game checks separately from publication tests.
The public [adoption verifier](../tests/project_gameplay_adoption_contract.py)
requires a prebuilt Traversal Yard Native AOT artifact and an owner-configured
profile. Supply its prepared course and installed runtime to include gameplay,
saves and relocated export inspection; the verifier does not build them.

On Windows, an external supervisor must assign the verifier and its descendants
to an owned Job before execution. `CHECKPOINT98_JOB_ASSIGNED=1` is the verifier's
admission marker; setting it alone does not establish ownership. Apply bounded
time/output limits and retain a naturally empty, unforced closure receipt.
The supervisor is outside these commands.

```sh
python tests/project_gameplay_adoption_contract.py \
  --binary runtime/bin/poima.exe \
  --artifact products/native-gameplay.json \
  --profile-template profiles.json --profile native \
  --prepared-world prepared/world.json --runtime runtime \
  --output adoption-proof --timeout 600
```

Use fresh output and compatible artifacts built from the documented
[Traversal Yard workflow](../examples/traversal-yard/README.md). Supplemental
recovery and controlled publication cohorts are recorded in the evidence;
their external qualification supervisors are not supplied by this verifier.
