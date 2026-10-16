# Editor selection against the native desktop bridge

This bounded Windows console fixture uses a separately built **actual**
`Poima.Editor.dll`: its production `NativeHost`, `EditorModel` and
`ComponentEditorModel` call an installed `bin/poima_desktop.dll`. There are no
linked replacement models, fake transports, Avalonia stubs or editor project
references. The fixture never builds the editor or native runtime.

Build with an installed .NET 10 SDK, supplying the complete previously qualified
editor binary directory explicitly:

```powershell
dotnet build tests/fixtures/editor_selection_native/Poima.SelectionNative.Contract.csproj -p:PoimaEditorDirectory=D:\qualification\editor\bin
```

Run the resulting console DLL with Windows .NET 10. All three directory
arguments must be absolute. The output directory must not exist; its parent must
already exist. Runtime means the install prefix containing `bin/poima_desktop.dll`
and its dependencies, such as `bin/phonon.dll`.

```powershell
dotnet Poima.SelectionNative.Contract.dll --editor-dir D:\qualification\editor\bin --runtime-dir D:\qualification\runtime --output D:\qualification\selection-native-new
```

The managed dependency resolver reads dependencies from the supplied editor
folder. The native resolver loads only the supplied installed desktop bridge,
using per-load Windows DLL-directory search flags for its dependencies. It does
not change global or machine DLL policy. The managed editor and native bridge
identities, managed resolution identities, case observations and a JSON report
are retained below the fresh output directory. Existing records are never
replaced. Each of four cases owns one small authored world with two box entities;
all authored mutations are confined to those worlds. No arbitrary commands,
network endpoints, external scene inputs or playback are accepted.

Cases cover dirty/invalid draft retention and Reload, successful Apply to original
A under its original revision before adopting deferred B, remote clear and deleted
pending selection, and a local selection acknowledgement followed by Refresh over
old cached host state. Selection-only checks compare complete authored on-disk
JSON and its byte hash, authoritative entity replies, world inspection and history.
Apply retains and checks the native transaction receipt, original target and
revision guard. An unattached gizmo inspection is recorded; no guessed gizmo
coordinates or gestures are attempted. Failed cases retain their errors and do not
prevent the remaining independent cases from running. Exit status is nonzero if
any case fails; the last stdout line is the complete JSON report, also written to
`report.json`.

There is no `AppBuilder`, window, foreground activation, native viewport attachment,
rendering, input injection or `NativeHost.Pump` clock loop. `NativeHost` is created
on the console's creating thread and retains its production
`Dispatcher.UIThread.VerifyAccess` check. If the actual Avalonia assemblies require
additional application initialization, the fixture fails with the actual exception;
it does not create a window or bypass that check.

This qualifies the actual managed model/native **unattached bridge** relationship
only. It cannot establish displayed Inspector/Hierarchy/gizmo agreement, presented
frames, physical input, docking or complete GUI behavior. A successful compile is
not an execution result; retain the original runtime report separately.
