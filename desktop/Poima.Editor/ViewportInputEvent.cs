// SPDX-License-Identifier: Apache-2.0
namespace Poima.Editor;

public enum ViewportInputKind
{
    PointerDown, PointerUp, PointerMove, PointerWheel,
    KeyDown, KeyUp, FocusLost, CaptureLost, Resized, RelativeMotion
}

public enum ViewportMouseButton { None, Left, Middle, Right, X1, X2 }

[Flags]
public enum ViewportMouseButtons { None = 0, Left = 1, Right = 2, Middle = 4, X1 = 8, X2 = 16 }

[Flags]
public enum ViewportModifiers { None = 0, Shift = 1, Control = 2, Alt = 4 }

public readonly record struct ViewportInputEvent(
    ViewportInputKind Kind,
    int X = 0, int Y = 0,
    ViewportMouseButton Button = ViewportMouseButton.None,
    ViewportMouseButtons Buttons = ViewportMouseButtons.None,
    int WheelDelta = 0, bool HorizontalWheel = false,
    uint VirtualKey = 0, bool Repeat = false, int ClickCount = 0,
    ViewportModifiers Modifiers = ViewportModifiers.None,
    int Width = 0, int Height = 0,
    int DeltaX = 0, int DeltaY = 0, uint ScanCode = 0,
    uint NativeScanCode = 0, bool ExtendedKey = false, bool Synthetic = false)
{
    public bool Shift => (Modifiers & ViewportModifiers.Shift) != 0;
    public bool Control => (Modifiers & ViewportModifiers.Control) != 0;
    public bool Alt => (Modifiers & ViewportModifiers.Alt) != 0;
}
