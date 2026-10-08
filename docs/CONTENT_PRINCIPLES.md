# Art, music and procedural tools

**Poima will not distribute generative-AI art or music as its own content. AI-assisted code is welcome.**

Engine examples, sample assets and promotional art follow this commitment. External assets need an identifiable source, suitable licensing and provenance consistent with it. This is Poima's content policy; the engine's Apache-2.0 license remains unchanged.

Agents should be able to discover and import existing free art and music from online sources, while developers can import their own assets. The planned acquisition workflow preserves the creator, source URL, exact license, required attribution and downloaded file identity. It checks the intended project's permitted use; a free download alone is not a license. Export should retain the relevant notices and credits.

Local asset import is available for the formats listed in the [implementation status](IMPLEMENTATION_STATUS.md). Online discovery/acquisition and automatic license records are planned features.

Procedural tools use explicit parameters and repeatable authored algorithms, without generative models producing images, meshes or music. The intended direction is helping developers work with existing assets: surface wear, dirt, erosion, chipped edges and controlled edge deformation, alongside reusable material layers and masks.

The current [brick and plaster recipes](PROCEDURAL_MATERIALS.md) provide a small algorithmic foundation. General wear tools and geometry deformation are not implemented yet. Surface appearance, rendered geometry and collision changes need distinct controls so visual aging does not silently change gameplay.
