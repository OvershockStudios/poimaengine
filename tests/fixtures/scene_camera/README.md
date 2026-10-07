# Scene camera motion contract

This package-free .NET 10 fixture links the production motion helper, navigation
state machine and viewport event types. Stand-in input and native-host objects
let it test time integration and cancellation without Avalonia, a GPU or Win32.
It does not qualify geometry picking, game cameras or physical input.

```text
dotnet run --project tests/fixtures/scene_camera/Poima.SceneCamera.Contract.csproj -c Release
```

Checks compare integrated flying at 30, 60 and 144 Hz, acceleration and stopping
distance, wheel accumulation/reversal/settling, zoom bounds, immediate mode and
cancellation on focus loss, capture loss, resize, scene changes and external
camera pose changes or cuts. The desktop regression remains the separate check
for the real HWND adapter, native camera and rendering integration.
