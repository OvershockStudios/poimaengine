# Poima desktop editor

Poima is designed for humans and agents. This first-class desktop editor uses Avalonia 12.1.3 and Dock 12.1.0.6 over the same native world service as the CLI and external agents. It remains an early implementation; headless configurations are also supported. See the [desktop guide](../../docs/DESKTOP_EDITOR.md) for build, launch and qualification details.

Implemented controls:

- Real Dock tabs, splits, separate floating windows and per-project saved arrangements. The default **Modified Tall** layout groups independent Scene and Game panels as tabs; **Modified Tall with Scene and Game** shows both together. Either panel can be moved or floated independently.
- Native Scene input: look/fly, pan/orbit/dolly, frame selection, geometry picking and move/rotate/scale gizmos through the shared bridge. Game has its own authored/runtime camera and viewport.
- Searchable hierarchical entity list with expansion, full-row striping, and original component-based vector icons.
- Guarded Inspector drafts: rename, typed Transform/Camera/MeshRenderer/PbrMaterial/Light/LightingEnvironment fields, and explicit JSON editing for other components. Invalid fields preserve the draft; stale Apply is rejected by the native revision guard. Reload explicitly discards the draft.
- RGB fields edit linear values; swatches display their sRGB conversion. Camera and light values show units. Global asset/physics/lighting constraints are still checked atomically by the core on Apply.
- A filesystem Project browser rooted at the nearest ancestor `project.json` (up to 16 levels), otherwise the world directory. Lazy directory expansion, list/grid modes, folder search, local glTF/GLB import, cooked model metadata, and model instantiation. Single-click selects a file/folder; double-click enters a folder or imports a source model.
- Shared native-session edits and undo/redo. **Play** starts real-time native simulation, **Pause/Resume** controls its clock, **Step** advances a paused fixed tick, and **Stop** discards runtime changes while preserving authoring. One host clock drives both viewports; rendering either pane does not advance simulation.
- During Play, click Game to engage a CharacterController camera’s keyboard/mouse input; Escape or Tab releases it. The Game toolbar selects a native input profile. Cameras without a controller remain previews. Editor gamepad input and audio output are not yet connected.
- An editable procedural sky with a linked directional Sun; new projects include both. Sky settings and world changes use the same guarded service operations as agent edits.

The browser excludes storage sidecars (`.lock`, `.pending`, `.previous`), symlinks/reparse points, and build/cache/version-control directories. Each folder lists at most 1,024 entries and expands at most 256 subfolders. Icons indicate asset types; they are not rendered asset thumbnails. Imported display names are currently session-local; previously cooked unnamed packages use a short hash label.

Still unqualified or not implemented: Unity UI parity, physical IME/accessibility behavior, multi-monitor/DPI edge cases, full drag/drop qualification, rendered asset thumbnails, material/texture pickers, animation/VFX authoring, editor extensions, and production-scale asset indexing. Native child-window viewport overlap/focus behavior requires explicit tests. Floating/resize/capture tests and screenshots must be reported separately from merely compiling the frontend.

Typography uses embedded Inter Regular/SemiBold. Icons are original vector paths; no Unity artwork is included. Every visible action must remain connected to a real operation; unsupported capabilities are described rather than represented by inert buttons.
