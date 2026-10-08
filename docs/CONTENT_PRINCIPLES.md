# Assets and content workflow

Developers can import their own assets. Planned discovery tools will also help
agents find and import existing content from online sources. Content retains
its own license; the engine's Apache-2.0 license does not change asset rights.

## Discovery, import and attribution

The intended workflow is to search a catalog, compare suitable licensed assets,
download a selected version, import it, and inspect the result in the game.
Search results should report format, size and compatibility alongside rights
and provenance information. Unknown information should stay explicit. When
search has no suitable result, the tools should report the gap.

[Asset records](ASSET_PROVENANCE.md) can preserve the creator, source URL,
selected license, supplied notices and declared input identities. Export retains
selected records and credits. Automatic acquisition should capture the actual
downloaded identities. User-owned files use the same import path. Import does not automatically
capture purchase receipts or local source paths. Selected record text is
included in exported credits.

Original downloads and cooked assets have separate identities. Conversion
records should preserve their relationship, including dependencies and
modifications. A file hash establishes byte identity, not authorship or
permission. A free download alone is not a license.

Local asset import is available for the formats listed in the
[implementation status](IMPLEMENTATION_STATUS.md). Online discovery/acquisition
and automatic source capture remain planned features.

## Procedural tools

The intended direction is helping developers work with existing assets:
surface wear, dirt, erosion, chipped edges and controlled edge deformation,
alongside reusable material layers and masks.

The current [brick and plaster recipes](PROCEDURAL_MATERIALS.md) provide a small
algorithmic foundation. General wear tools and geometry deformation are not
implemented yet. Surface appearance, rendered geometry and collision changes
need distinct controls so visual aging does not silently change gameplay.
