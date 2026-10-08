# Authored asset references

`world.asset.references` identifies the authored entity or template fields that reference cooked models, images and audio. It supports both directions: find the users of an asset, or list one owner's dependencies. It reads the current loaded authoring document, including while a runtime is active. It does not read packages, decode models, rebuild snapshots or enumerate live spawned owners.

This is an evolving development API, outside selected authoring-core v1. Discovery revision 54 adds the method; protocol 1 and the selected stable contract remain unchanged.

## Query

All selectors are optional on a first page:

```json
{"jsonrpc":"2.0","id":1,"method":"world.asset.references","params":{"asset":"<64 lowercase hex asset hash>","limit":64}}
```

For forward references, replace `asset` with `owner`:

```json
{"jsonrpc":"2.0","id":2,"method":"world.asset.references","params":{"owner":{"kind":"template","id":"<32 lowercase hex template ID>"}}}
```

`asset` and `owner` are mutually exclusive. Omit both to list all explicit authored references. Owner kind is `entity` or `template`; the namespaces are distinct even when their textual IDs match. A known owner without references and a well-formed unused asset hash return an empty page. An unknown owner is an error. Null selectors are invalid.

Optional `revision` pins the loaded authored revision. `limit` is an integer 1–256, default 64. Unknown object fields, malformed identifiers, Boolean/fractional numeric fields and incompatible selectors reject.

## Result and pages

```json
{
  "revision":7,
  "scope":"authored",
  "selection":{"asset":"<64 lowercase hex asset hash>"},
  "edges":[
    {
      "owner":{"kind":"entity","id":"<32 lowercase hex entity ID>"},
      "component":"PbrTextures",
      "path":"/base_color/asset",
      "asset":"<64 lowercase hex asset hash>",
      "kind":"model",
      "subresource":{"kind":"image","index":2}
    }
  ],
  "next_after":null
}
```

Each edge identifies a field location and its declared package kind. `subresource` is null or an authored primitive/image index; the operation does not verify that the index exists. `selection` echoes the asset filter, owner filter or `{}`. Names, filesystem source paths and unrelated component values are not returned.

Edges sort by `(owner.kind, owner.id, component, path)` in bytewise lexical order, with entities before templates. Repeated use of one asset through different fields remains distinct. `next_after` is null unless another matching edge exists, including when the last page exactly fills the requested limit. Otherwise it contains the last returned field location:

```json
{
  "revision":7,
  "asset":"<same hash>",
  "after":{
    "owner":{"kind":"entity","id":"<last returned owner ID>"},
    "component":"PbrTextures",
    "path":"/base_color/asset"
  },
  "limit":64
}
```

Continue with the returned revision and the same selector. `after` is an exclusive lexical boundary and requires `revision`; it need not identify an existing edge. A competing edit makes an old revision fail. The cursor contains no retained snapshot or authentication capability and is not bound cryptographically to a selector. Changing a selector with the same cursor creates a different query. Cursor paths must be one of the typed field locations below; arbitrary JSON Pointer evaluation is not supported.

## Typed references

| Component and field | Package kind | Subresource |
| --- | --- | --- |
| `AnimationRig.asset` | model | null |
| `AudioEmitter.asset` | audio | null |
| `MeshCollider.asset` | model | authored primitive |
| `StaticMesh.asset` | model | authored primitive |
| `SkinnedMesh.asset` | model | authored primitive |
| `PbrTextures.<slot>.asset`, without `image` | image | null |
| `PbrTextures.<slot>.asset`, with `image` | model | authored embedded image |

Direct component cursors use `/asset`. Texture slots are `base_color`, `emissive`, `metallic_roughness`, `normal` and `occlusion`; their cursor paths are `/<slot>/asset`. Templates currently admit only the StaticMesh and PbrTextures reference rows.

Invisible meshes, collision-only geometry, disabled emitters and frozen template references are included. Omitted texture slots inherit imported maps; null slots disable them. Neither creates another explicit edge. Imported material images, skins, curves and layer/reference clips live inside the referenced model package; this operation does not decode them into an effective material graph. Use [effective material inspection](MATERIAL_AUTHORING.md#inspect-the-effective-material) when that is needed.

Entity camera/rig/sun handles, custom component values, UI text/actions, hash-shaped strings, receipts, undo history and retired bookkeeping are not inferred to be assets.

## Resolution, scope and bounds

The existing `world.dependencies` operation remains the resource-validation/export closure. It loads referenced packages, checks content and returns a deduplicated file inventory. This reference query reports authored bindings even when the package is missing or corrupt; successful observation does not establish availability, color space, primitive/image validity or renderability. Effective inspection, runtime construction and export retain their existing validation errors.

The method is available to standalone, shared-headless, shared-editor and read-only packaged sessions. All observe one owner-serialized loaded document revision; an active runtime does not redirect the read into its frozen definition. The operation does not reread external world-file edits, write receipts/history/storage, modify caches or advance simulation. Ordinary diagnostic tracing can record the read.

Current loaded documents permit 10,000 entities and 256 templates. The conservative typed scan bound is 101,536 references; pages retain at most `limit + 1` records. Results are limited to 1 MiB of serialized UTF-8 result JSON before the JSON-RPC wrapper; overflow fails without truncation. These bounds do not establish maximum-content performance or a process-memory ceiling.

Malformed requests and invalid continuation shape use `-32602`; stale revision uses `-32009`; unknown owner uses `-32004`. An unused valid hash is an empty result, not a resource-resolution error.

## Verification

The pure collector target is `poima-asset-references-test`; the native protocol fixture is `tests/asset_references_contract.py`, accepting a positional executable, `--runtime 0|1` and optional `--windows-interop`. Fixtures target independent expected edges, typed exclusions, sorted exclusive pagination, guard/error preservation, shared/read-only scope and agreement with the existing dependency inventory for valid resources.

Windows and Linux runtime builds and an authoring-only Linux build each pass eight protocol groups, including real model/image/audio imports, missing/corrupt resources, exclusive pages and a competing shared-session edit. Runtime builds also preserve the frozen gameplay state during authored reads and changes. Native WorldSession checks cover shared-editor and read-only exposure; the MCP suite checks discovery, results and stale errors. [Recorded results](evidence/m2-asset-references.json).

Source-file tracking, unused-file deletion, import-job publication, persistent graph caches, live spawned-resource graphs, FBX import and editor reference panels remain separate work.
