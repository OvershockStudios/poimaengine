# Compiled character-input fixture

This test game drives a camera-free capsule through the native character service.
Its authoritative fields select movement, neutral ticks and intentional failures.
The contract harness runs the same module through CoreCLR and NativeAOT, checking
collision, player-input ownership, same-tick reads, batch rollback, reload and saves.
