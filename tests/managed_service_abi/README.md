# Managed services ABI contract

This package-free executable references an already built managed bridge and SDK. It does not rebuild production projects. It declares the C ABI independently, invokes the real bridge, and loads a stateless fixture through its collectible assembly loader.

```sh
dotnet build tests/managed_service_abi/ManagedServiceAbi.csproj -c Release -o build/managed-service-abi
dotnet build/managed-service-abi/ManagedServiceAbi.dll build/managed-animation-abi-linux.json
```

Override `-p:BridgeDirectory=/absolute/path/to/bridge-directory` to qualify a different built bridge/SDK pair. The same output DLL can run under native Windows .NET 10; supply Windows paths and a separate evidence destination.

The checks reject incompatible service versions/sizes and absent callbacks before gameplay or native callbacks run, verify animation POD field transfer and nullable/frozen metadata, and verify fixture context collection. Evidence includes bridge and SDK hashes. This supplements native engine integration tests; it does not replace physics/animation batch rollback, reload continuity, or gameplay command timing qualification.
