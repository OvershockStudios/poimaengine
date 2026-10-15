# Live material editing

An agent can author a surface change, then explicitly apply it to the running
game without recreating its runtime or calling gameplay initialization again.
The adopted material becomes part of that runtime's frozen save baseline.
Ordinary authoring edits remain separate until applied.

This API is outside the stable authoring-core v1 contract. It supports changes
to `base_color`, `emissive`, `metallic` and `roughness` on existing authored
drawables with an explicit `PbrMaterial`. A request selects 1–32 entity IDs in
ascending order. Every selected material must contain a scalar change.

## Author, inspect, apply

1. Use `world.transact` to set the complete authored `PbrMaterial`, retaining
   its `double_sided` value and all other authored content.
2. Read `runtime.inspect` for the live tick, structure/material revisions,
   frozen authored revision and `content_sha256`. The authored transaction's
   returned revision identifies the target.
3. Call `runtime.material.apply` with those exact guards and selected IDs.
4. Read `runtime.material.inspect` for the accepted values. Use player or
   capture receipts to establish which immutable snapshot was presented.

The apply request is:

```json
{
  "session_id": "00000000000000000000000000000001",
  "request_id": "00000000000000000000000000000002",
  "expected_tick": 140,
  "expected_structure_revision": 0,
  "expected_material_revision": 0,
  "revision": 6,
  "expected_authored_revision": 5,
  "expected_content_sha256": "<SHA-256 returned by runtime.inspect>",
  "ids": ["00000000000000000000000000000003"]
}
```

The response returns `session_id`, `tick`, the incremented `material_revision`,
adopted `authored_revision`, new `content_sha256`, `ids` and `replayed`.
An exact retained retry returns the original result without applying twice,
even after the game has advanced. Reusing its request ID with different
parameters is an error. Runtime mutation receipts retain the last 32 operations;
simulation and structural operations also reserve their own bounded receipts
in the same runtime request namespace.

`runtime.material.inspect` accepts `session_id`, exact `tick`, sorted `ids`
and an optional exact `material_revision`. It returns the accepted native
material values and both frozen/current authored revisions. It does not
advance simulation, poll the player or render a frame.

## State and persistence

Apply preserves the runtime session, tick, physics state, controllers,
animation, custom components, gameplay module and UI state. Existing snapshots
keep their original material values and immutable mesh/texture owners.
The selected scalar values, frozen document, content identity and authored
revision change together; the material observation epoch increments once.

After that epoch becomes nonzero, `save.write` and active `save.load` require
`expected_material_revision`. `runtime.capture` requires `material_revision`;
`player.capture` requires `expected_material_revision`. Supplied guards are
always checked, including at epoch zero. Stopped restore accepts an absent or
null material guard. Retained save/capture retries resolve before current-state
conflicts.

Material values persist in the checkpoint's frozen document. The observation
epoch is local to a runtime session and resets to zero on restoration. Loading
an older checkpoint restores its older material baseline and can report
`source_stale` relative to current authoring. It does not silently adopt current
material edits. Undo and redo likewise require a separate explicit apply.

## Accepted, submitted and presented

The mutation receipt proves native acceptance. It does not prove that a frame
has been drawn. `player.inspect` retains a `report.materials` section with
`observed`, `submitted` and `presented` receipts. Each includes the runtime
session, snapshot tick, player generation and immutable scene identity. Frame
receipts also identify the actual context frame and submission.

Capture results expose the captured material epoch and
`render_diagnostics.scene_receipts`. Compare the entire identity, rather than
the epoch alone, across player or runtime replacement. Skipped or failed
presentation preserves the prior successful presented receipt. A successful Vulkan
presentation acknowledgement does not establish GPU completion or physical
display scanout.

The material epoch invalidates temporal scene history. The visual qualification
scope for each rendering/reconstruction configuration is recorded separately.

## Rejections and limits

- Unrelated authored changes, unselected material edits, new or removed
  components, geometry, textures, samplers, `double_sided`, visibility and
  shadow-control changes reject the entire batch.
- Missing, spawned or retired drawable IDs cannot be selected. Hidden authored
  drawables can be changed, but are not claimed visibly presented.
- A pending gameplay save, including a dispatched background write, must be
  resolved before adoption.
- Preparation revalidates the complete old and target asset/provenance
  closures. It performs synchronous file/CPU work; this is not a guarantee of
  hitch-free texture or shader reload.
- Failed preparation preserves the live baseline and does not consume a
  material receipt. The already committed authored edit remains available for
  correction or undo.

Interactive player sessions admit material adoption at the serialized owner
boundary, including during playback. Exact tick guards can reject an edit when
the game advances before admission. Recorded replay forbids intervening edits.
Texture replacement and general live scene adoption need separate qualification.

## Qualification and reproduction

The [0.0.113 results](evidence/m2-live-materials.json) cover eight Windows native
checks, schema-82 discovery, 42 standalone captures and ten same-window captures
on one AMD GPU. Forward and deferred comparisons match their authored references
exactly. The weighted fixtures use a bind pose; they do not qualify every animated
character or rendering configuration. AO and reconstruction are disabled.

A retained CoreCLR Relay Yard game applies a material change at tick 140, keeps
its complete saved state apart from the adopted authored/content identity,
finishes its objective and restores the adopted baseline in a fresh process.
Native window tests separately check receipt routing and recovery. Fixture wall
times are execution bounds, not edit-to-visible latency measurements.

Run `poima-world-material-test` and the runtime/save/player suites from a
simulation-enabled build. The public [capture fixture](../tests/live_material_capture.py)
creates its own deterministic scene and assets; the
[player fixture](../tests/live_material_player.py) uses one shared native window.
Both take `--binary`, a fresh `--output`, `--lighting-path forward|deferred`
and `--gpu`. Use native Windows Python and an externally owned Windows Job so
failed tests cannot leave native descendants running. The
[retained-game fixture](../tests/relay_yard_material_iteration.py) additionally
requires the recorded original checkpoint, its cooked assets, compiled game and
CoreCLR bridge; it is a historical continuation check rather than a standalone
sample download.

Use the separate [texture API](LIVE_TEXTURES.md) for map, sampler and strength
changes. Both operations share the same material revision and observation/save
guards; scalar apply remains scalar-only.

### Representative scalar iteration

The [0.0.158 cohort](evidence/m2-live-material-iteration.json) meets the selected
one-second scalar-edit budget on the laptop's RTX 4070: **0.574 s mean,
0.660 s p95**, across all twenty edits. Nearest-rank p95 is sample 19 after
sorting; no slow samples are removed. This is a paused Windows native player
using the retained Performance Yard game, rather than an empty material fixture.

The workload keeps 2,394 authored entities, 24 animated characters, 768 box
props, 64 lights and 32 dynamic bodies. It uses the existing overview camera,
1080p native rendering, deferred lighting, medium GTAO, one sample, two frames
in flight and an enabled audio backend. Frame generation and reconstruction
are disabled. Audio closure is checked; this is not a listening qualification.

Each edit changes the complete scalar material on all 24 selected skins and
then explicitly adopts it through `runtime.material.apply`. The primary timer
starts before the first edit-specific file read. It includes authoring, guarded
adoption, observation of a matching presented receipt, a fresh guarded capture,
complete image verification and retained difference masks. The first matching
presentation observation has a separate timer; neither timer measures scanout.

Expected complete authoring bytes and reference-image masks prepare outside
the primary interval. Authoring preparation has its own recorded timing: 0.431 s mean and 0.561 s
p95. Including preparation, the separate preparation-start-to-verification-end
measurement is 1.005 s mean and 1.209 s p95; it is not the primary gate. Every
actual pre-edit world, previous-file backup and post-edit world must match its
complete expected bytes; the native transaction receipt must match exactly.
Unknown capture bytes take the full RGB comparison path inside the timer.
Exact reference-byte matches permit reuse of independently verified masks.
The original and changed references differ in 282,659 pixels outside the HUD
at a minimum channel delta of eight. Every fresh image matches its selected
reference bytes exactly.

After each timed interval, a complete native checkpoint is compared with both
the preceding checkpoint and the baseline. All version-7 payload fields remain
exact except `authored_revision` and `content_sha256`, which identify the
adopted material baseline. The frozen saved document is checked separately
against the full expected content. Both cameras, all 50 gameplay values,
physics, animation, components, sound, structure and UI remain present.
The final edit restores the original material values without replacing the
runtime or player.

Exclusive BMP output now reserves a bounded frame-sized SDL memory chunk,
avoiding repeated 1 KiB growth. Five isolated Windows CPU pairs produce
identical complete BMP bytes with both allocation strategies. The fresh
cohort also changes the verifier's authoring-byte preparation. Its earlier
2.304 s p95 result remains a missed budget; the end-to-end results do not
isolate either change's contribution.

Reproduce the operation sequence using the
[Performance Yard preparation](../examples/performance-yard/README.md), its
compiled gameplay artifact, one shared player and the existing overview
camera `00000000000000000000000000000066`. Initialize the game through its
compiled New Game control, settle 60 ordinary ticks after the initial tick,
then keep tick 61 paused. Freeze original and changed material references
before timing; alternate those complete material values twenty times with
fresh transaction/apply/capture IDs and exact current guards. Keep every
capture, checkpoint, receipt, timing sample and failure. The recorded cohort
uses the specific retained 0.0.94 artifact and pinned input identities in the
evidence; rebuilding with other content or tooling is a separate cohort.

This qualifies one scalar workload and device. Encoded PNG texture-import
timing, other GPUs, advancing-game edits, physical input, general scene edits,
frame performance and the broader Alpha gates remain separate.
