# Make a guarded scene edit

This playbook demonstrates a small authored hierarchy, an intervening edit,
stale-edit rejection, deliberate reconciliation, retained retry receipts and
fresh-owner persistence. It needs no renderer, .NET runtime or downloaded art.
It creates authoring data; it does not create a playable game or rendered mesh.

## Run the example

Build the [headless engine](../../docs/BUILD.md) and install the
[Python client](../../docs/PYTHON_CLIENT.md):

```sh
python3 -m pip install ./tools/python
python3 examples/guarded-authoring/edit_scene.py \
  build/headless/poima build/guarded-example/world.json
```

Use a new world path on every run. Python and the executable must be native to
the same operating system. The recorded example runs on Linux; it does not
translate WSL paths to Windows paths. To use a runtime-enabled Linux build,
substitute `build/runtime-headless/poima`.

Success prints `"passed": true` and the completed checks. Exceptions or native
errors fail the run. Both standalone owners close and reap their engine process.
[Recorded qualification](../../docs/evidence/m2-guarded-authoring-playbook.json)
uses an installed client in a fresh Python environment.

## Workflow

1. Discover focused `entity.create` and Transform mutation schemas. Read the
   current authored revision before preparing changes.
2. Preview one transaction creating Workshop and its Crate child. Verify that
   preview did not commit, then submit the same operations as one guarded edit.
3. Rename Crate. This simulates another edit between reading and submitting;
   the example uses one owner rather than launching simultaneous clients.
4. Submit a Transform edit using the earlier revision. Expect `-32009` and
   verify that the world stayed unchanged.
5. Inspect the changed entity, deliberately accept its new name, and submit a
   new edit against the current revision. A stale error is a reason to reconcile,
   not permission to blindly overwrite with a refreshed revision.
6. Resend the original creation payload with its original receipt. Verify
   `replayed: true`, the original result revision and no new authored revision.
7. Close and reopen the same world. Verify the final entity and revision, then
   verify that the original receipt still replays without creating duplicates.

## Recovery and limits

This script deliberately exercises a terminal conflict, whose result is known.
An interrupted request can instead have an unknown outcome. Preserve its exact
method, payload and receipt; follow the [client recovery contract](../../docs/PYTHON_CLIENT.md#failure-and-deliberate-recovery)
before sending another mutation. Do not regenerate a receipt and assume it is
the same request. Receipts retain only the latest 128 committed edits.

For an open editor, connect through its [shared session](../../docs/SHARED_SESSIONS.md)
instead of opening a competing standalone writer. Closing a shared client
detaches it; the editor retains ownership. Undo is session-wide, so inspect
history before reverting another author's edit.

The [authoring-core v1 reference](../../docs/AUTHORING_API_COMPATIBILITY.md)
defines the stabilized boundary. Broader runtime and rendering APIs have their
own contracts. This example does not establish visual fidelity, simultaneous
human/agent editing or recovery from every transport failure.
