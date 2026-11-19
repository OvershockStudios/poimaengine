# Compiled held-item release

This fixture uses the simulation-enabled Poima 0.0.259 host and .NET 10.
The managed callback attempts an obstructed release, then a semantic drop
input releases the item. Both attempts request a save in their own tick.
A separately stepped native owner supplies the contact witness and moving
holder's final pose. Later fresh owners replay each save through the
settled state. The test also checks duplicate rejection and rollback of a
release after an invalid later callback in the same step.

## CoreCLR development

From the repository root, build the bridge and game:

```sh
dotnet build managed/Poima.ManagedBridge -c Release -o build/release-bridge
dotnet build tests/fixtures/item_attachment_release_gameplay -c Release -o build/release-game
```

Point `--hostfxr` to the .NET 10 hostfxr library for the installed SDK.
Use a new output directory for each run.

```sh
python3 tests/item_attachment_release_gameplay.py --binary build/linux-game/poima \
  --hostfxr /path/to/dotnet/host/fxr/10.0.x/libhostfxr.so \
  --bridge build/release-bridge/Poima.ManagedBridge.dll \
  --assembly build/release-game/Poima.ItemAttachmentReleaseGame.dll \
  --output build/release-game-check
```

## Windows Native AOT

Use a Windows developer shell with the MSVC linker available:

```bat
python scripts/publish_native_gameplay.py --project tests/fixtures/item_attachment_release_gameplay/Poima.ItemAttachmentReleaseGame.csproj --type Poima.Tests.AttachmentReleaseGame --output build/release-aot --work build/release-aot-work
python tests/item_attachment_release_gameplay.py --binary build/windows-runtime/bin/poima.exe --descriptor build/release-aot/native-gameplay.json --gpu 1 --output build/release-aot-check
```

Use the GPU index for your Vulkan adapter. The scenario also runs without the
`--gpu` option. Add `--windows-interop` if executing the Python test through
WSL with a Windows binary and descriptor. For either backend, pass
`--previous-binary` with a compatible retained 0.0.258 executable to check
feature negotiation at the same services extent.

The fixture leaves the authored world intact and records requests, responses,
whole save archives and optional Vulkan captures in the selected directory.
[Release API](../../../docs/ITEM_ATTACHMENTS.md#release-from-c-gameplay)
