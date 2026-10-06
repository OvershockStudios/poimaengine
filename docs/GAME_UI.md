# Native game UI foundation

Poima has two native UI foundations: authoritative logical controls in the world/runtime, and retained document layout with shared Vulkan composition. They are not connected to one another yet. Compiled C# callbacks, automatic layout binding and live player/desktop input routing remain unfinished.

The optional `POIMA_ENABLE_GAME_UI` build uses pinned RmlUi 6.3 and FreeType. It does not enable Lua, browser code or third-party scripting. The logical model is available in the default headless engine; it has no RmlUi, font or graphics dependency. Existing Inter font files are redistributed under their retained OFL license; see [third-party notices](../THIRD_PARTY_NOTICES.md).

## Authoritative controls

Authored world format 4 adds a separate `ui` hierarchy. Panels, labels and buttons have nonzero 32-character lowercase hexadecimal IDs, independent of world entities. Controls do not require transforms or physics objects. Definitions are frozen at runtime start and participate in packaged content identity.

Use `world.transact` with `ui.element.set` or `ui.element.remove`. A set operation supplies the complete element:

```json
{
  "op": "ui.element.set",
  "id": "10000000000000000000000000000001",
  "element": {
    "parent": null,
    "name": "Health",
    "kind": "label",
    "text": "Health 100",
    "action": null,
    "visible": true,
    "enabled": true
  }
}
```

Parents must be panels. Panel text is empty. Buttons require an action token of 1–128 ASCII letters, digits, underscores, dots or hyphens; other kinds require `action:null`. Tokens are metadata only: a button named Save does not write a checkpoint. Final-state validation permits a child before its parent within the same transaction. Removing a parent requires removing or reparenting its children in that transaction. Retired IDs cannot be reused outside known undo/redo history.

`world.ui.get` and `world.ui.list` inspect authored definitions. Pagination uses `revision`, `after` and `limit`. `runtime.ui.inspect` accepts `session_id`, `tick`, optional `ui_revision`, `after` and `limit`. Continuation pages require `ui_revision`. It returns frozen metadata alongside live text, local and inherited visibility/enabled state, modal membership eligibility and the next cursor. Inspection does not search for pixels or require a window.

`runtime.ui.edit` commits all supplied patches at the current simulation tick:

```json
{
  "session_id": "20000000000000000000000000000001",
  "request_id": "30000000000000000000000000000001",
  "expected_tick": 0,
  "expected_ui_revision": 0,
  "edits": [
    {"id": "10000000000000000000000000000001", "text": "Health 83"}
  ]
}
```

Each patch needs an ID and at least one of `text`, `visible` or `enabled`. Duplicate IDs are rejected. Optional `modal` selects an effectively visible panel; explicit `null` clears it, and omission preserves it. A modal-only edit can use an empty `edits` array. Hiding the active modal requires clearing or replacing it in the same transaction. Buttons are logically eligible only when they and their ancestors are visible/enabled and they lie inside the active modal, if any. Eligibility does not execute an action or promise a physically clickable layout region.

An accepted edit advances `ui_revision` once, including a same-value edit; it leaves simulation tick, physics and authored defaults unchanged. Invalid edits publish nothing. Identical retries return the original result within the shared 32-receipt runtime window, even after later UI/tick changes. A new runtime session invalidates old edit requests. Desktop-hosted edits require paused Play. The API is serialized by the native owner; it is not a concurrent direct-memory interface.

Definitions allow 256 controls, hierarchy depth 32, 128 UTF-8 bytes per name, 16 KiB per text and 1 MiB total text. Text is literal UTF-8 without NUL. These are bounded contract limits, not performance claims. The [native model API](../include/poima/ui_model.hpp) validates sorted complete definitions and owns mutable values. It is available without simulation; a live `Runtime` additionally requires the simulation build.

UI-bearing runtimes use snapshot version 4. Saves preserve complete text/visibility/enabled state, active modal and UI revision against trusted frozen definitions; no focus, atlas or pixel data is serialized. Existing UI-free snapshots retain versions 1–3. External `save.write` and replacement `save.load` require `expected_ui_revision` when the active world contains UI. The Save/Load editor model retains this guard and recognizes same-tick UI edits as stale. Stopped restore accepts an absent or null guard.

## Presentation boundary

The native document adapter lays out in-memory RML and produces an owned, immutable `UiFrame`. It exposes registered label/button identities separately from rendered pixels. It returns semantic action identifiers to its caller; the document does not execute gameplay, write saves or advance simulation.

Geometry, textures, transforms and scissor rectangles cross the renderer boundary as native data. The common renderer composes UI after resolving scene MSAA and before capture. Legacy ImGui presentation uses the resolved scene texture, so game UI does not cover editor chrome. The first composition path requires an sRGB attachment and explicitly rejects unsupported UNORM composition.

RML/RCSS is RmlUi's layout format, not browser HTML/CSS. For a fixed container that must clip absolutely positioned children, use `overflow: hidden; clip: always;`. The adapter uses the library's clipping and hit testing. Semantic activation currently searches a bounded grid for an actionable point; very thin, partly occluded or transformed controls can report `hittable=false` despite having a small clickable region. This restriction must be resolved before claiming general control parity.

Input colors and textures use premultiplied sRGB bytes. Conversion to linear premultiplied values happens before texture filtering and framebuffer blending. Text uses rasterized glyph coverage. This does not establish production text accessibility, bidirectional/complex-script shaping, localization or geometric edge quality for every transform.

## Building

For native layout checks without a graphics dependency:

```sh
cmake -S . -B build/game-ui -G Ninja \
  -DPOIMA_ENABLE_VULKAN_PROBE=OFF \
  -DPOIMA_BUILD_MODULE_LAB=OFF \
  -DPOIMA_ENABLE_GAME_UI=ON
cmake --build build/game-ui --target poima-ui-document-test poima-ui-frame-test -j2
ctest --test-dir build/game-ui -R '^ui_(document|frame)_native$' --output-on-failure
```

Dependency downloads are verified by SHA-256. Renderer checks additionally require the existing Vulkan build dependencies and an accessible graphics device. Layout tests alone do not qualify rendering.

[The presentation example](../examples/ui/hud.rml) contains status text and two buttons. Its action names are illustrative: loading this document does not implement Continue or Save behavior. The host must bind returned actions to its own authoritative command path.

## Native API

[UiDocument](../include/poima/ui_document.hpp) accepts document text, owned font bytes and an explicit list of registered labels/buttons. Font families are shared within the library lifetime; case aliases must resolve to identical font bytes. Resource references cannot read disk or fetch URLs. Inline scripts, data-binding expressions, external images/stylesheets and unimplemented rendering effects are rejected.

```cpp
poima::UiDocumentSource source;
source.rml = document_text;
source.fonts.push_back({"Poima UI", font_bytes});
source.elements = {
    {"health", poima::UiElementKind::label, {}},
    {"continue", poima::UiElementKind::button, "continue"},
};
poima::UiDocument ui(std::move(source));
ui.set_text("health", "Health 83 / 100"); // Literal text, never markup.
scene.ui = ui.frame(960, 540, 1.0f, presentation_seconds);
auto controls = ui.inspect();
auto requested_action = ui.activate("continue");
```

`frame` takes physical pixel dimensions, density scale and nondecreasing presentation time. Published frames remain valid after document destruction. `set_visible` and `set_enabled` affect action validation; `pointer_activate`, `activate`, `focus_next` and `activate_focused` return presentation results without executing game code. Platform event wiring remains the caller's responsibility. A registered element cannot contain another registered element, because replacing its text would invalidate the descendant binding.

Current bounds include a 1 MiB RML document, 4,096 parsed nodes, 256 registered elements, 16 fonts per source and 16 KiB replacement text. [Draw-packet validation](../include/poima/ui.hpp) bounds extent, vertices, indices, draws and texture storage. Limit failures report errors instead of publishing invalid packets. These bounds are guardrails, not a measured production workload budget.

## Current limits

Both APIs are experimental. Native logical controls and document presentation remain separate: no automatic layout/style binding, C# UI callbacks, UI action execution or control-turn replay is implemented. Returned presentation action strings do not pause/resume a game or request storage operations. Controller navigation integration, inventory controls, text input, comprehensive accessibility and the UI editor remain unfinished.

## Recorded checks

[Logical-state evidence](evidence/m2-native-ui-state.json) covers native model/runtime checks, authored and runtime RPC transactions, strict restores, legacy save regressions and a linked C# Save model test. Windows and Linux both pass the authored six-case and runtime four-case UI suites. The default authoring-only build also validates UI definitions without simulation or RmlUi. Reproduce the focused native checks with `poima-ui-model-test` and `poima-runtime-ui-test`; the RPC harnesses are `tests/world_ui_contract.py` and `tests/runtime_ui_contract.py`. Each accepts a CLI binary and `--runtime 0` or `--runtime 1`; Windows interop uses `--windows-interop`. The save-panel model test runs with `.NET 10` using `dotnet run --project tests/fixtures/editor_save_model/Poima.SaveModel.Contract.csproj`.

[Qualification evidence](evidence/m2-native-game-ui.json) covers native layout/packet tests on Windows and Linux, the default headless build, and explicit Windows Vulkan captures on the laptop's AMD and NVIDIA GPUs. Numeric readback checks cover alpha composition, textures, transforms and clipping at 1× and 4× scene MSAA. The example also passes layout/action checks at 100% and 150% scale.

These checks do not qualify a live player/editor UI workflow, physical input, console support, a deployment package or production performance.

![Native HUD presentation fixture over the engine sky](evidence/m2-native-game-ui.png)

*Actual Vulkan capture of the presentation fixture. Text and focus are set through the native document API; the buttons are not connected to gameplay or storage.*
