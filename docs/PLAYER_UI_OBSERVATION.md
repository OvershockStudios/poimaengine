# Observe displayed game UI

`player.ui.inspect` reads geometry and presentation flags from the native UI
layout that reached a live player's last successful presentation. Use it to
find clipping, check actual font/DPI scaling and relate controls to a screenshot.
It executes no action and does not require OCR.

This complements two existing reads: `runtime.ui.inspect` supplies the complete
logical hierarchy, text, eligibility and authored layout/style; `world.ui.layout`
computes an authored virtual viewport without live focus or scroll history.
See [game UI](GAME_UI.md) and [the live player](LIVE_PLAYER.md).

## Discover and read

Use a shared authored-world host or [exported game service](GAME_SERVICE.md)
with an active native player and game UI support. Discover the method, inspect
the player, and use the returned identity, generation and current session:

```json
{"jsonrpc":"2.0","id":1,"method":"world.describe","params":{"view":"method","name":"player.ui.inspect"}}
{"jsonrpc":"2.0","id":2,"method":"player.inspect","params":{}}
{"jsonrpc":"2.0","id":3,"method":"player.ui.inspect","params":{"player_id":"00000000000000000000000000000901","generation":1,"session_id":"00000000000000000000000000000101"}}
```

These are separate newline-delimited requests, not a JSON-RPC batch. Example
IDs and values throughout this guide are illustrative; replace them with actual
responses. This method is outside [authoring-core v1](AUTHORING_API_COMPATIBILITY.md).

`player_id`, `generation` and `session_id` are required exact ownership guards.
`tick` and `ui_revision` are independent optional optimistic guards. Omit both
when observing running gameplay: the host can advance between requests, so a
previously read Tick may already be stale. For a paused, exact-state check:

```json
{"jsonrpc":"2.0","id":4,"method":"player.ui.inspect","params":{"player_id":"00000000000000000000000000000901","generation":1,"session_id":"00000000000000000000000000000101","tick":42,"ui_revision":8}}
```

A supplied guard must match exactly; omission does not weaken mutation guards.
Read-only inspection uses no `request_id` and creates no retry receipt.

## Current state and displayed state

The response always reports the current owner tags observed atomically during
the request. `displayed` contains independently tagged presentation data:

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "format": "poima.player-ui-observation.v1",
    "player_id": "00000000000000000000000000000901",
    "generation": 1,
    "session_id": "00000000000000000000000000000101",
    "tick": 42,
    "ui_revision": 8,
    "displayed": {
      "session_id": "00000000000000000000000000000101",
      "tick": 41,
      "ui_revision": 7,
      "player_generation": 1,
      "frame_sequence": 20,
      "submission_id": 37,
      "layout_generation": 5,
      "viewport": [0, 0, 960, 540],
      "width": 960,
      "height": 540,
      "scale": 1.25,
      "matches_current_owner": false,
      "controls": [
        {
          "id": "00000000000000000000000000000003",
          "kind": "button",
          "bounds": [30, 525, 255, 570],
          "clip": [0, 0, 960, 540],
          "font_size_px": 20,
          "visible": true,
          "enabled": true,
          "hittable": true,
          "focused": false
        }
      ]
    }
  }
}
```

Here the exact current guards succeeded, but the displayed layout belongs to
an older Tick/UI revision. `matches_current_owner` is true only when the retained
sample belongs to the current session, Tick, player generation and immutable
logical UI projection, and the player reports ready. It is not an OS visibility,
physical-input or monitor scanout measurement.

Successful normal frames and captures that present can replace the sample.
New preparation, edits, runtime replacement, failed frames and skipped frames
do not relabel an old sample as current. Replaced sessions may therefore retain
an explicitly older displayed session until another successful presentation.
`displayed: null` means no UI observation is retained, including before a first
UI presentation or after presenting without logical UI. A stopped player has
no terminal UI-inspection interface; inspect again after a new player starts.

`frame_sequence` and `submission_id` identify the renderer presentation receipt.
`layout_generation` identifies the prepared UI layout within that presenter;
it can change for presentation input without a new logical `ui_revision`.
These counters are identities, not timing or latency measurements.

## Pixels, clipping and DPI

Controls appear in authored visual traversal order. Each row is a label or
button; panels remain available through logical inspection.

| Field | Meaning |
| --- | --- |
| `bounds` | Untransformed border rectangle `[left, top, right, bottom]` in physical viewport-local pixels. |
| `clip` | Ancestor scissor intersected with the viewport, in the same coordinates. An empty intersection has no visible area. |
| `viewport` | UI viewport rectangle in render-target pixels; add its left/top offset to local coordinates when relating them to the full captured image. |
| `width`, `height` | Physical dimensions used for this UI layout. |
| `scale` | Actual absolute UI density used for this sample. |
| `font_size_px` | Computed inherited font size in physical pixels. |
| `visible`, `enabled`, `focused` | Native presentation flags for this sample. Logical eligibility is inspected separately. |
| `hittable` | An enabled visible button has at least one successful sampled native hit point. Labels report false. |

For example, at scale 1.25, authored `top: 420dp`, `height: 36dp` and
`font_size: 16dp` yield 525..570 physical pixels and a 20-pixel computed font.
In a 540-pixel viewport, only the top 15 pixels of that border rectangle lie
inside the viewport. A partially clipped button can still be hittable.

Explicit UI scale is absolute; it is not multiplied by window display density.
Without an override, the sample follows the effective window density. Compare
the returned scale with [live settings](PLAYER_SETTINGS.md), not just the
requested preference. Percentage dimensions resolve against their containing
layout; fixed dp widths can overflow a small logical viewport at higher density.

Border and scissor rectangles are not glyph ink, text line bounds, transformed
coverage, rounded clip masks or occlusion-area estimates. Text can overflow its
own background unless its layout clips it. Interpret a rectangle with
`right <= left` or `bottom <= top` as empty rather than inventing a positive area.

## Agent recipe: screenshot and logical state

1. Read `player.inspect`, then call `player.ui.inspect` with its exact identity,
   generation and current session. While running, omit Tick/UI guards and read
   the returned current and displayed tags together.
2. For an exact committed-state inspection, pause through a fresh guarded
   `player.control` request. Inspect readiness and the current session again;
   a historical pause receipt does not establish current state.
3. Read `runtime.ui.inspect` with the paused session, Tick, UI revision and
   `limit: 256`. This supplies text, parents, authored styles and eligibility.
   Read `player.ui.inspect` with the same optional Tick/UI guards. Wait for a
   current presentation if `matches_current_owner` is false.
4. Capture through `player.capture` at that exact paused Tick, or
   `player.capture_current` without a Tick parameter. Use a fresh request ID,
   new output path, exact player/control/session and applicable UI, structure
   and material guards. These are separate capture operations; inspection
   itself never captures or advances simulation.
5. Inspect the image, capture receipt and displayed controls together. Match
   session/Tick/UI tags before attributing the image to logical state. For an
   exact renderer-frame association, also match displayed `frame_sequence` and
   `submission_id` against `capture.render_diagnostics.scene_receipts.presented`.
   Separate RPCs are not an atomic image/layout transaction: even a paused
   host can present another frame or process focus/scroll input between them.
   If frame identities differ, report the shared committed state boundary
   without claiming those rectangles came from the exact captured frame.
   Normal presentation can advance between reads; a different frame identity
   alone does not mean capture failed.
6. Make corrections through guarded operations. `runtime.ui.edit` supports live
   text, visibility, enabled and modal changes; authored layout/style changes
   require a new runtime's frozen definition. Follow the player/runtime restart
   lifecycle before observing that new definition. Trigger gameplay through the
   separate [compiled control API](GAME_UI.md#compiled-control-callbacks), not
   this read.

The native document freezes observations after its layout update and before
rendering. The retained immutable data owns its lifetime; reading it performs
no DOM update, focus/scroll change, action callback, Tick, GPU drain or device
query. The host can still perform its ordinary frame pump between requests.

## Rejection and limits

| Condition | Error |
| --- | --- |
| Missing, malformed or unknown fields; non-integer/Boolean revision guard | `-32602` |
| Unknown or replaced player identity | `-32004` |
| Stale generation/session, or stale supplied Tick/UI revision | `-32009` |
| No live native player or UI presentation support | `-32003` |

The response contains at most 256 label/button rows. Hittability uses the
existing deterministic 5×5 grid within the clipped border rectangle and actual
native hit tests. Pointer-event policy still applies: a pass-through overlay
can cover pixels while allowing hits beneath it. Hittability does not measure
visible area. Thin, transformed or partially occluded controls can have valid
pointer locations missed by that sample; `hittable: false` is not proof that
every possible pointer point fails. This is not a physical mouse/controller or
accessibility qualification. See [implementation status](IMPLEMENTATION_STATUS.md)
for the recorded build and platform scope.

## Recorded qualification

The [0.0.132 record](evidence/m2-player-ui-observation.json) covers native Windows
layout/lifetime tests, an AMD shared player with compiled controls and save/load,
and 32 unpaused NVIDIA replay observations while native C# updates the UI. Linux
authoring/non-rendering compilation, discovery, MCP and build metadata also pass.
The interactive observation interval remained paused; focused interactive input
and representative observation cost remain open.

The replay test accepts an existing engine and the retained managed-UI Native AOT
fixture; it does not compile or modify them:

```powershell
python -B tests/player_ui_replay_observation.py --binary PATH_TO_POIMA_EXE `
  --native-descriptor PATH_TO_MANAGED_UI_NATIVE_GAMEPLAY_JSON `
  --output NEW_OUTPUT_DIRECTORY --gpu 1
```

Use native Windows Python and the owned native-test gate when collecting release
evidence. Select a real GPU index; the index above refers to the recorded laptop.
Replay avoids the interactive focus requirement without claiming physical input.
