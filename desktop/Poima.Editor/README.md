# Desktop frontend prototype

This optional C# frontend uses Avalonia 12.1.3 and Dock 12.1.0.6. Poima's native world service remains authoritative; editor appearance does not change the headless game/runtime architecture.

Implemented controls:

- Real Dock tabs, splits, separate floating windows, and layout reset. Default hosts use document tabs so each panel has one top header, including Project/Console.
- Searchable hierarchical entity list with expansion, full-row striping, and original component-based vector icons.
- Guarded Inspector drafts: rename, typed Transform/Camera/MeshRenderer/PbrMaterial/Light fields, and explicit JSON editing for other components. Invalid fields preserve the draft; stale Apply is rejected by the native revision guard. Reload explicitly discards the draft.
- RGB fields edit linear values; swatches display their sRGB conversion. Camera and light values show units. Global asset/physics/lighting constraints are still checked atomically by the core on Apply.
- A filesystem Project browser rooted at the nearest ancestor `project.json` (up to 16 levels), otherwise the world directory. Lazy directory expansion, list/grid modes, folder search, local glTF/GLB import, cooked model metadata, and model instantiation. Single-click selects a file/folder; double-click enters a folder or imports a source model.
- Shared native-session edits, undo/redo, simulation start paused, fixed-tick Step, and Stop. No automatic real-time stepping is implied by the start control.

The browser excludes storage sidecars (`.lock`, `.pending`, `.previous`), symlinks/reparse points, and build/cache/version-control directories. Each folder lists at most 1,024 entries and expands at most 256 subfolders. Icons indicate asset types; they are not rendered asset thumbnails. Imported display names are currently session-local; previously cooked unnamed packages use a short hash label.

Still unqualified or not implemented: Unity UI parity, physical IME/accessibility behavior, multi-monitor/DPI edge cases, full drag/drop qualification, persisted dock layouts, rendered asset thumbnails, material/texture pickers, animation/VFX authoring, editor extensions, and production-scale asset indexing. Native child-window viewport overlap/focus behavior requires explicit tests. Floating/resize/capture tests and screenshots must be reported separately from merely compiling the frontend.

Typography uses embedded Inter Regular/SemiBold, now explicitly accepted by the user to match Unity’s design guidance. Icons are original vector paths; no Unity artwork is included. Every visible action must remain connected to a real operation; unsupported capabilities are described rather than represented by inert buttons.
