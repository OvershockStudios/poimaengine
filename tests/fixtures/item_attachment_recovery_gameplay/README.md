# Recoverable sampled grips

The fixture uses a moving skinned ribbon and a held rigid item. It commits a
surface miss, reads the outcome in C#, and saves and restores the same
tick in a fresh owner. It also verifies an invalid command rollback, a frame
miss, a later successful transfer, and a strict command in the same tick as
a recoverable miss.

```sh
dotnet build managed/Poima.ManagedBridge -c Release -o build/recovery-bridge
dotnet build tests/fixtures/item_attachment_recovery_gameplay -c Release -o build/recovery-game
python3 tests/item_attachment_recovery_gameplay.py --binary build/linux-game/poima \
  --hostfxr /path/to/libhostfxr.so --bridge build/recovery-bridge/Poima.ManagedBridge.dll \
  --assembly build/recovery-game/Poima.ItemAttachmentRecoveryGame.dll \
  --output build/compiled-recovery-check
```

For Native AOT, publish the project with `scripts/publish_native_gameplay.py`
using type `Poima.Tests.AttachmentRecoveryGame` and pass its descriptor via
`--descriptor` in place of the three CoreCLR arguments. Use
`--windows-interop` for a Windows executable launched from WSL.
