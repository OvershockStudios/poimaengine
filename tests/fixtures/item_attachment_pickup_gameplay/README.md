# Sampled item pickup

The fixture uses original procedural ribbon geometry. It drops an item onto a
static floor, samples the moving grip and tests retirement of the actual
dynamic body. It compares collision queries, visual ownership and complete
save archives in fresh runtime processes. The compiled scenario also exercises
a semantic pickup input, a failed reach, a live reference that blocks
retirement, and rollback after a later invalid command.

## Native service

Build a simulation-enabled Poima 0.0.260 host. From the repository root, run:

```sh
python3 tests/item_attachment_pickup.py --binary build/linux-game/poima \
  --output build/pickup-service-check
```

Use a new output directory. On Windows Vulkan, add `--gpu` with your adapter
index to see the item leave the world view and return to the foreground
pass. Add `--windows-interop` when starting a Windows executable from WSL.

## CoreCLR gameplay

Use the .NET 10 SDK to build the bridge and game:

```sh
dotnet build managed/Poima.ManagedBridge -c Release -o build/pickup-bridge
dotnet build tests/fixtures/item_attachment_pickup_gameplay -c Release -o build/pickup-game
```

Point `--hostfxr` to the hostfxr library from your installed .NET 10 runtime.

```sh
python3 tests/item_attachment_pickup.py --binary build/linux-game/poima \
  --hostfxr /path/to/dotnet/host/fxr/10.0.x/libhostfxr.so \
  --bridge build/pickup-bridge/Poima.ManagedBridge.dll \
  --assembly build/pickup-game/Poima.ItemAttachmentPickupGame.dll \
  --output build/pickup-game-check
```

## Windows Native AOT gameplay

Use a Windows developer shell with the MSVC linker available:

```bat
python scripts/publish_native_gameplay.py --project tests/fixtures/item_attachment_pickup_gameplay/Poima.ItemAttachmentPickupGame.csproj --type Poima.Tests.AttachmentPickupGame --output build/pickup-aot --work build/pickup-aot-work
python tests/item_attachment_pickup.py --binary build/windows-runtime/bin/poima.exe --descriptor build/pickup-aot/native-gameplay.json --gpu 1 --output build/pickup-aot-check
```

Pass `--previous-binary` with a retained 0.0.259 host to exercise rejection
of the new capability at the same service-table extent.

## Relocated standalone game

Use the authored world from the fixture above, its published Native AOT
descriptor, and an [installed Windows runtime](../../../docs/PROJECTS.md).
The verifier creates a temporary project, adopts the module on Windows,
exports a game and moves the source project aside. It then runs the bundled
service, verifies same-tick pickup and reloads the saved grip in a fresh
game process.

```bat
python tests/item_attachment_pickup_export.py --world build/pickup-aot-check/authored/world.json --artifact build/pickup-aot/native-gameplay.json --runtime build/installed-windows --output build/pickup-export-check
```

Use `--windows-interop` for the same workflow from WSL.

A passing run keeps the release collider after a missed grip, retires it with
the accepted grip and replays the same complete archives after restart.
[Pickup API](../../../docs/ITEM_ATTACHMENTS.md#pick-up-a-released-item)
