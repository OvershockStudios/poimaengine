# Procedural material recipes

These original JSON recipes use the native brick/plaster baker and ordinary PBR texture assets. `wine-brick.recipe.json` creates a muted wine-colored brick wall with pale mortar; `warm-plaster.recipe.json` creates a warm, fine-grained plaster surface. Both are Apache-2.0 data.

Pass a recipe object to `asset.material.generate`, poll `asset.material.job`, then apply its returned `PbrMaterial` and `PbrTextures` through a guarded `world.transact`. Use `asset.material.inspect` to recover the canonical recipe after reopening. See [the full API and channel conventions](../../docs/PROCEDURAL_MATERIALS.md).

The brick tile covers 2 m×1 m per UV repeat; plaster covers 2 m×2 m. Match geometry UVs/physical scale explicitly. Generation does not edit selected entities, create geometry, or change active runtime images.
