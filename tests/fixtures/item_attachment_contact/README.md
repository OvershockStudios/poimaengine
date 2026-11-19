# Guarded item contact fixture

<!-- SPDX-License-Identifier: Apache-2.0 -->

The compiled lock checks the animated anchor after the movement that occurs
in the same tick. The harness compares direct and compiled transfer results,
position and angle errors, rejection of a late command in a batch, same-tick
save publication, and complete cold continuation.

```sh
dotnet build tests/fixtures/item_attachment_contact -c Release -o build/contact-game
dotnet build managed/Poima.ManagedBridge -c Release -o build/contact-bridge
python3 tests/item_attachment_contact.py --binary build/linux-game/poima \
  --hostfxr /path/to/libhostfxr.so --bridge build/contact-bridge/Poima.ManagedBridge.dll \
  --assembly build/contact-game/Poima.ItemAttachmentContactGame.dll \
  --output build/contact-check
```

To use [Native AOT](../../../docs/NATIVE_GAMEPLAY.md), publish the project with
`--type Poima.Tests.AttachmentContactGame`. Pass `--descriptor` to the harness
in place of the CoreCLR arguments.

The imported case uses the unchanged [CC0 Knight](../../../docs/CHARACTER_LIBRARY.md#original-source)
and an independent calibration from the glTF ready pose. It verifies a missed
transfer during motion, an aligned keep-world transfer, and continued grip
tracking on the receiving hand.

```sh
python3 tests/imported_attachment_contact.py --binary build/linux-game/poima \
  --source /path/to/Knight.glb --output build/imported-contact-check
```

Use `--windows-interop` for a Windows executable launched from WSL. Add `--gpu`
with an adapter index for offscreen Vulkan captures of the same frames. Use
new output folders to retain the original requests, results and captures.
