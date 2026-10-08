# Asset sources, licenses and credits

Poima can keep immutable source and license records alongside cooked assets.
They are caller declarations: a hash verifies record bytes, not authorship,
licensing permission or the accuracy of a source URL. Assets without records
remain usable. These operations perform no catalog search or network download.

## Create and select records

Import a supported asset through the [asset API](ASSETS.md), then call
`asset.provenance.create` with `record`:

```json
{
  "format": "poima.asset-provenance",
  "version": 1,
  "asset": "<cooked asset SHA-256>",
  "kind": "image",
  "title": "Stone surface",
  "creator": "Asset creator",
  "source": "https://example.org/assets/stone",
  "license": {
    "identifier": "Custom permission",
    "notice": "Exact supplied license and attribution text."
  }
}
```

`kind` is `image`, `model`, `audio` or `navigation`. Creation checks the existing
cooked package's kind and content hash. The response identifies a canonical
UTF-8 JSON `.pprov` record by SHA-256, with `bytes` and `created`. Repeating the
same record reuses it. Changing credits creates a different record without
changing the cooked asset. Inspect with `asset.provenance.inspect` and
`{"record":"<record SHA-256>"}`.

Select records with the ordinary guarded `world.transact` operation:

```json
{
  "op": "asset.provenance.set",
  "asset": "<cooked asset SHA-256>",
  "records": ["<record SHA-256>"]
}
```

Up to eight unique records may describe one cooked asset, including different
source declarations for identical imported bytes. IDs are sorted on admission.
Records must name that asset and agree on its cooked kind. An empty `records`
array removes the selection. Related scene edits and selections commit
atomically. Preview, retained retry receipts, revision conflicts and bounded
undo/redo follow the [world service](WORLD_SERVICE.md) contracts. Record creation
writes immutable assets but does not change the world or its revision.

## Inspect, save and export

`world.asset.provenance` requires `revision`. An optional `asset` selects one
mapping; otherwise `limit` (1–64, default 64) and `after` page in asset-ID order.
The result has `assets`, `has_more` and `next_after`. A targeted read cannot have
`after`. This reads authored metadata only, including when a record file is
missing; use record inspection or `world.dependencies` for fresh byte validation.

Frozen runtimes, save restoration and project exports resolve records from the
source world they are using. A save made with selection A retains A even if
current authoring selects B. Missing, corrupt or linked records reject content
validation before a replacement runtime is activated. Undo/redo restores
metadata selections; subsequent content validation checks their files.

All explicitly selected records remain in the frozen document and export,
including selections for unused assets. This preserves deliberate credits and
keeps every frozen record reference resolvable. An unused mapping does not add
its cooked asset to the bundle. Unselected records, receipts and old history are
not dependencies. Unmapped worlds retain their existing content identity and
bundle shape.

Exports inventory `.pprov` files separately as `asset_provenance` and write
`content/ASSET_CREDITS.txt` as `asset_credits`. Credits retain every selected
notice in deterministic asset/record order. Verification regenerates credits
from the checked records and compares exact bytes, in addition to checking the
bundle inventory. Updating an inventory hash alone cannot legitimize altered
credits. Bundles with records require a runtime explicitly advertising
`features.asset_provenance`; unmapped worlds retain support for older runtimes.
The engine does not decide which of several license declarations is
applicable or combine them into a legal conclusion.

## Bounds and remaining work

Each canonical record is at most 64 KiB. Text fields reject NUL characters.
Title and creator are 1–1,024 UTF-8
bytes; license identifier is 1–256 and notice 1–32,768 bytes. Optional `source`
is an HTTP(S) citation of at most 4,096 bytes, without URL credentials,
ASCII whitespace/control characters or backslashes. Its syntax is checked; reachability and ownership
are not. Records use closed fields and reject duplicate JSON keys.

Optional `inputs` holds up to 32 unique `{"sha256":"<hash>","bytes":123}`
identities. Sizes are integers from zero through 2^53−1. These are caller-declared
original inputs, not an importer-verified dependency trace. A glTF source can
have external images and buffers; declaring its top-level hash does not prove
those inputs. A world has at most 10,000 mappings. Frozen record bytes and
credits are each bounded to 16 MiB, in addition to existing world and bundle
limits. Packaged read-only worlds reject creation and authoring edits.

Automatic source capture, catalog connectors, download validation and richer
music imports remain separate work. The current [asset workflow](CONTENT_PRINCIPLES.md)
and [implementation status](IMPLEMENTATION_STATUS.md) describe supported formats.

## Reproduce the checks

Use a native build with tests enabled. The record and package tests also work
without simulation; `--runtime 1` additionally exercises frozen-save
restoration when simulation is enabled.

```sh
cmake --build build/runtime-headless --target poima poima-asset-provenance-test poima-save-upgrade-document-test -j 1
build/runtime-headless/poima-asset-provenance-test
build/runtime-headless/poima-save-upgrade-document-test
python3 tests/asset_provenance_contract.py build/runtime-headless/poima --runtime 1
python3 tests/asset_provenance_project_contract.py build/runtime-headless/poima
```

The project tests use real native image imports and a deliberately
non-executable runtime fixture. They test export and verification, including
relocation and altered notices, rather than player deployment.
