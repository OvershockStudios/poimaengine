# Editor layout storage contract

This package-free .NET 10 executable links the production `EditorLayoutStore.cs`
directly. It does not reference Avalonia, Dock, the native engine, or a test
framework. Local `NuGet.Config` clears package sources; the pinned SDK supplies
the framework references.

From the repository root:

```sh
DOTNET_CLI_HOME="$PWD/.cache/dotnet-home" NUGET_PACKAGES="$PWD/.cache/nuget" \
  .cache/toolchains/dotnet-10.0.401/dotnet run \
  --project tests/fixtures/editor_layout_store/Poima.LayoutStore.Contract.csproj \
  -c Release -- --output build/editor-layout-store-contract
```

Each invocation creates a unique evidence directory containing JSON results and
test-owned preference files. It never writes default user preferences or game
projects. Cases cover custom paths, project identities, tree/tab/floating bounds
round trips across six panels, Modified Tall presets, version-1 migration that inserts Game beside Scene without rewriting on load, replacing a valid file while an old reader retains its original
bytes, rejected malformed/unsupported/duplicate/oversize/nonfinite data, and
preservation of existing bytes after both load and save failures.

To qualify Windows file sharing and replacement behavior, publish this same
small executable with `-r win-x64 --self-contained true -o <new-output-dir>` and
run its `.exe` with a Windows `--output` path. The cached Windows .NET runtime
packs must already exist because this project's package sources are disabled.
Neither contract loads a GUI or accesses real editor preferences.

This qualifies storage behavior only. Native floating-window restoration,
display clamping, and actual GUI interaction require the desktop tests.
