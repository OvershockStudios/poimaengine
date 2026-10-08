# Managed services ABI contract

This package-free executable references an already built managed bridge and SDK. It does not rebuild production projects. It declares the C ABI independently, invokes the real bridge, and loads a stateless fixture through its collectible assembly loader.

```sh
dotnet build tests/managed_service_abi/ManagedServiceAbi.csproj -c Release -o build/managed-service-abi
dotnet build/managed-service-abi/ManagedServiceAbi.dll build/managed-animation-abi-linux.json
```

Override `-p:BridgeDirectory=/absolute/path/to/bridge-directory` to qualify a different built bridge/SDK pair. The same output DLL can run under native Windows .NET 10; supply Windows paths and a separate evidence destination.

The checks require services epoch 7 with a known 176-byte baseline prefix. Longer tables (177, 192 and 256 bytes) execute Tick and Control while opaque extension bytes remain untouched. Eight-byte headers placed against an inaccessible page prove short tables and wrong epochs are rejected before callback slots are read. Every baseline callback remains required even on larger tables. Input validation and the exact call epoch 1/80-byte layout remain enforced. They verify animation/save POD transfer, generated component query/get/set/liveness, and spawn/despawn/frozen-template component callbacks, then run 100 additional collectible module lifetimes. Lifecycle assertions cover distinct template IDs, nullable and explicit transform transfers, reserved entity IDs, template presence flags and native error propagation. Component assertions independently verify all five field kinds, sorted query cursors, missing-component results, fingerprint words, canonical padding, finite checks and positive-zero encoding. Evidence includes bridge and SDK hashes. This supplements native engine integration tests; it does not replace physics/animation batch rollback, reload continuity, or gameplay command timing qualification.

The opt-in animation extension tests independently declare its 64-byte command, 136-byte state and 192-byte service table. They exercise the real CoreCLR bridge and public SDK with mock native callbacks: marked load requirements, pre-constructor host rejection, actual 176/184-byte tables against protected pages, either missing tail callback, both transition modes, nullable mode/progress, the unchanged seven-argument setter and 14 malformed returned states. A constructor that deliberately throws provides a positive sentinel for the load guard; failed contexts must retire. These tests do not send malformed PODs into the engine's actual runtime callbacks or qualify animation rollback.

`NativeEntry/` links the production native entry point with a small test-only typed binding, exercising the baseline guards through its unmanaged entry under CoreCLR on Windows and Linux. This is not NativeAOT publication or old-artifact compatibility qualification. The SDK's baseline service struct remains 176 bytes; the animation extension is a separate negotiated table.
