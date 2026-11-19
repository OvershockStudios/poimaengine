# Compiled item attachment fixture

<!-- SPDX-License-Identifier: Apache-2.0 -->

The fixture uses two animated sockets, a rigid foreground item with a child,
and an idle kinematic collider. It checks the donor's uninterrupted sample
against the same tick after a compiled handoff. Subsequent ticks follow the
receiver. It also exercises pin, snap, reset, atomic failure, same-tick save
requests, and full checkpoint equality after a fresh-process restore.

With a .NET 10 SDK and a simulation/managed gameplay build, compile the fixture
and bridge, then run the engine through the public service:

```sh
dotnet build tests/managed_item_attachment_gameplay -c Release -o build/attachment-game
dotnet build managed/Poima.ManagedBridge -c Release -o build/attachment-bridge
python3 tests/item_attachment_gameplay.py --binary build/linux-game/poima \
  --hostfxr /path/to/libhostfxr.so --bridge build/attachment-bridge/Poima.ManagedBridge.dll \
  --assembly build/attachment-game/Poima.ManagedItemAttachmentGame.dll \
  --output build/compiled-attachment-check
```

For the [Native AOT route](../../docs/NATIVE_GAMEPLAY.md), publish the same
project with `--type Poima.Tests.ItemAttachmentGame`. Pass `--descriptor`
instead of the three CoreCLR arguments. The unmarked consumer runs in the
CoreCLR route, where one assembly can select either game type.
For a Windows executable from WSL, add `--windows-interop`. Add `--gpu` and
an adapter index to compare offscreen captures. Output directories must be
new and retain the requests, responses, checkpoints, and any captures.
