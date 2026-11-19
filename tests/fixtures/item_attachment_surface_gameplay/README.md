# Compiled sampled grips

The fixture stages a sampled handoff in the first tick and a second
handoff after the skin moves away from the item. It verifies a full batch
rollback, same-tick save, and exact cold continuation after repairing the
request. The source fixture contains a weighted ribbon and a rigid target.

```sh
dotnet build managed/Poima.ManagedBridge -c Release -o build/surface-bridge
dotnet build tests/fixtures/item_attachment_surface_gameplay -c Release -o build/surface-game
python3 tests/item_attachment_surface_gameplay.py --binary build/linux-game/poima \
  --hostfxr /path/to/libhostfxr.so --bridge build/surface-bridge/Poima.ManagedBridge.dll \
  --assembly build/surface-game/Poima.ItemAttachmentSurfaceGame.dll \
  --output build/compiled-surface-check
```

To exercise Native AOT, publish the same project with
`scripts/publish_native_gameplay.py --type Poima.Tests.AttachmentSurfaceGame`
and supply its `native-gameplay.json` through `--descriptor` in place of the
three CoreCLR arguments. Use `--windows-interop` for a Windows executable
launched from WSL.
