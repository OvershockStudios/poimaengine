// SPDX-License-Identifier: Apache-2.0
using System.ComponentModel;
using System.Runtime.InteropServices;
using Avalonia.Threading;

namespace Poima.Editor;

public enum ViewportInputKind
{
    PointerDown, PointerUp, PointerMove, PointerWheel,
    KeyDown, KeyUp, FocusLost, CaptureLost, Resized
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
    int Width = 0, int Height = 0)
{
    public bool Shift => (Modifiers & ViewportModifiers.Shift) != 0;
    public bool Control => (Modifiers & ViewportModifiers.Control) != 0;
    public bool Alt => (Modifiers & ViewportModifiers.Alt) != 0;
}

/// <summary>
/// Local input for an owned Windows viewport child. Create and dispose on its UI
/// thread. Dispose BEFORE SDL detaches or the NativeControlHost destroys the HWND.
/// This uses the common-controls subclass chain, preserving SDL's window handling.
/// Pointer coordinates and dimensions are physical client pixels (negative pointer
/// coordinates are possible while captured). Wheel deltas retain Windows' 120-unit
/// convention. Double clicks depend on the native window class's CS_DBLCLKS flag.
/// </summary>
public sealed class ViewportInput : IDisposable
{
    // Windows does not retain managed delegates. This table deliberately roots both
    // the subscriber and its callback until successful removal or WM_NCDESTROY.
    private static readonly Dictionary<nuint, ViewportInput> Active = new();
    private static readonly Native.SubclassProc Callback = WindowProc;
    private static long nextId;
    private readonly nuint id;
    private readonly Action<ViewportInputEvent> onEvent;
    private IntPtr window;
    private ViewportMouseButtons buttons;
    private bool suppressCaptureNotification;
    private bool resetting;
    private int pointerX;
    private int pointerY;

    public int Width { get; private set; }
    public int Height { get; private set; }
    /// <summary>Latest caught callback/Win32 error; exceptions never escape WindowProc.</summary>
    public string? LastError { get; private set; }

    public ViewportInput(IntPtr hwnd, Action<ViewportInputEvent> onEvent)
    {
        Dispatcher.UIThread.VerifyAccess();
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Viewport HWND input requires Windows.");
        ArgumentNullException.ThrowIfNull(onEvent);
        var thread = Native.GetWindowThreadProcessId(hwnd, out var process);
        if (hwnd == IntPtr.Zero || thread == 0 || thread != Native.GetCurrentThreadId() || process != (uint)Environment.ProcessId)
            throw new ArgumentException("The viewport must be an owned window on the current UI thread.", nameof(hwnd));
        this.onEvent = onEvent;
        window = hwnd;
        id = checked((nuint)Interlocked.Increment(ref nextId));
        RefreshSize();
        Active.Add(id, this);
        if (!Native.SetWindowSubclass(window, Callback, id, 0))
        {
            Active.Remove(id);
            window = IntPtr.Zero;
            throw new InvalidOperationException("SetWindowSubclass failed for the viewport.");
        }
    }

    /// <summary>End local drags and clear consumer key state, without stealing another HWND's capture.</summary>
    public void CancelCapture()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (window == IntPtr.Zero) return;
        Reset(ViewportInputKind.CaptureLost, true);
    }

    public void Dispose()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (window == IntPtr.Zero) return;
        CancelCapture();
        // A subscriber may synchronously destroy the native child in CancelCapture.
        if (window == IntPtr.Zero) return;
        if (!Native.RemoveWindowSubclass(window, Callback, id))
        {
            // Keep the managed root on failure: a leak is safer than a dangling
            // unmanaged callback. WM_NCDESTROY remains a cleanup fallback.
            LastError = "RemoveWindowSubclass failed for the viewport.";
            throw new InvalidOperationException(LastError);
        }
        window = IntPtr.Zero;
        Active.Remove(id);
        GC.SuppressFinalize(this);
    }

    private static IntPtr WindowProc(IntPtr hwnd, uint message, nuint wparam, nint lparam, nuint subclassId, nuint referenceData)
    {
        // No exception, including a consumer's event exception, can cross this ABI.
        try
        {
            if (Active.TryGetValue(subclassId, out var input))
            {
                if (message == 0x0082) // WM_NCDESTROY: fallback if owner did not dispose.
                {
                    input.Reset(ViewportInputKind.FocusLost, true);
                    Native.RemoveWindowSubclass(hwnd, Callback, subclassId);
                    input.window = IntPtr.Zero;
                    Active.Remove(subclassId);
                }
                else input.Process(message, wparam, lparam);
            }
        }
        catch (Exception error)
        {
            if (Active.TryGetValue(subclassId, out var input)) input.Fail(error);
        }
        // Always preserve SDL/default processing; this adapter does not consume keys.
        return Native.DefSubclassProc(hwnd, message, wparam, lparam);
    }

    private void Process(uint message, nuint wparam, nint lparam)
    {
        switch (message)
        {
            case 0x0201: case 0x0204: case 0x0207: case 0x020B: // button down
            case 0x0203: case 0x0206: case 0x0209: case 0x020D: // double click
            {
                ReadPoint(lparam);
                var button = MouseButton(message, wparam);
                if (button == ViewportMouseButton.None) return;
                Native.SetFocus(window); // Deliberate clicks only; never hover/wheel.
                if (window == IntPtr.Zero) return;
                Native.SetCapture(window);
                if (window == IntPtr.Zero) return;
                buttons |= Mask(button);
                Emit(new(ViewportInputKind.PointerDown, Button: button,
                    ClickCount: message is 0x0203 or 0x0206 or 0x0209 or 0x020D ? 2 : 1));
                break;
            }
            case 0x0202: case 0x0205: case 0x0208: case 0x020C:
            {
                ReadPoint(lparam);
                var button = MouseButton(message, wparam);
                if (button == ViewportMouseButton.None) return;
                buttons &= ~Mask(button);
                Emit(new(ViewportInputKind.PointerUp, Button: button));
                if (buttons == ViewportMouseButtons.None) ReleaseOwnedCapture();
                break;
            }
            case 0x0200:
                ReadPoint(lparam);
                Emit(new(ViewportInputKind.PointerMove));
                break;
            case 0x020A: case 0x020E:
            {
                var point = new Native.Point { X = SignedLow(lparam), Y = SignedHigh(lparam) };
                if (!Native.ScreenToClient(window, ref point)) throw new Win32Exception(Marshal.GetLastWin32Error());
                pointerX = point.X; pointerY = point.Y;
                Emit(new(ViewportInputKind.PointerWheel,
                    WheelDelta: unchecked((short)((wparam >> 16) & 0xffff)), HorizontalWheel: message == 0x020E));
                break;
            }
            case 0x0100: case 0x0104:
                Emit(new(ViewportInputKind.KeyDown, VirtualKey: (uint)wparam,
                    Repeat: (((long)lparam >> 30) & 1) != 0));
                break;
            case 0x0101: case 0x0105:
                Emit(new(ViewportInputKind.KeyUp, VirtualKey: (uint)wparam));
                break;
            case 0x0008: // WM_KILLFOCUS
                Reset(ViewportInputKind.FocusLost, true);
                break;
            case 0x001F: // WM_CANCELMODE
                Reset(ViewportInputKind.CaptureLost, true);
                break;
            case 0x0215: // WM_CAPTURECHANGED, including voluntary ReleaseCapture.
                if (!suppressCaptureNotification) Reset(ViewportInputKind.CaptureLost, false);
                break;
            case 0x0005: case 0x02E3: // WM_SIZE / WM_DPICHANGED_AFTERPARENT
                RefreshSize();
                Emit(new(ViewportInputKind.Resized));
                break;
        }
    }

    private void Reset(ViewportInputKind reason, bool release)
    {
        if (resetting) return;
        resetting = true;
        buttons = ViewportMouseButtons.None;
        try
        {
            if (release)
            {
                suppressCaptureNotification = true;
                try { ReleaseOwnedCapture(); }
                finally { suppressCaptureNotification = false; }
            }
            Emit(new(reason));
        }
        finally { resetting = false; }
    }

    private void ReleaseOwnedCapture()
    {
        if (window != IntPtr.Zero && Native.GetCapture() == window && !Native.ReleaseCapture())
            LastError = "ReleaseCapture failed for the viewport.";
    }

    private void Emit(ViewportInputEvent input)
    {
        if (window == IntPtr.Zero) return;
        try
        {
            onEvent(input with { X = pointerX, Y = pointerY, Buttons = buttons,
                Modifiers = ReadModifiers(), Width = Width, Height = Height });
        }
        catch (Exception error) { Fail(error); }
    }

    private void Fail(Exception error)
    {
        LastError = error.ToString();
        buttons = ViewportMouseButtons.None;
        // Suppress recursive notifications if a subscriber itself failed. Consumers
        // should surface LastError and cancel their own navigation state on error.
        var previous = suppressCaptureNotification;
        suppressCaptureNotification = true;
        try { ReleaseOwnedCapture(); }
        catch (Exception cleanupError) { LastError += "\nCapture cleanup: " + cleanupError.Message; }
        finally { suppressCaptureNotification = previous; }
    }

    private void RefreshSize()
    {
        if (!Native.GetClientRect(window, out var rect)) throw new Win32Exception(Marshal.GetLastWin32Error());
        Width = Math.Max(0, rect.Right - rect.Left);
        Height = Math.Max(0, rect.Bottom - rect.Top);
    }

    private void ReadPoint(nint lparam) { pointerX = SignedLow(lparam); pointerY = SignedHigh(lparam); }
    private static int SignedLow(nint value) => unchecked((short)((long)value & 0xffff));
    private static int SignedHigh(nint value) => unchecked((short)(((long)value >> 16) & 0xffff));
    private static ViewportModifiers ReadModifiers() =>
        ((Native.GetKeyState(0x10) & 0x8000) != 0 ? ViewportModifiers.Shift : 0) |
        ((Native.GetKeyState(0x11) & 0x8000) != 0 ? ViewportModifiers.Control : 0) |
        ((Native.GetKeyState(0x12) & 0x8000) != 0 ? ViewportModifiers.Alt : 0);

    private static ViewportMouseButton MouseButton(uint message, nuint wparam) => message switch
    {
        >= 0x0201 and <= 0x0203 => ViewportMouseButton.Left,
        >= 0x0204 and <= 0x0206 => ViewportMouseButton.Right,
        >= 0x0207 and <= 0x0209 => ViewportMouseButton.Middle,
        >= 0x020B and <= 0x020D => ((wparam >> 16) & 0xffff) switch
        { 1 => ViewportMouseButton.X1, 2 => ViewportMouseButton.X2, _ => ViewportMouseButton.None },
        _ => ViewportMouseButton.None
    };

    private static ViewportMouseButtons Mask(ViewportMouseButton button) => button switch
    {
        ViewportMouseButton.Left => ViewportMouseButtons.Left,
        ViewportMouseButton.Right => ViewportMouseButtons.Right,
        ViewportMouseButton.Middle => ViewportMouseButtons.Middle,
        ViewportMouseButton.X1 => ViewportMouseButtons.X1,
        ViewportMouseButton.X2 => ViewportMouseButtons.X2,
        _ => ViewportMouseButtons.None
    };

    private static class Native
    {
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        internal delegate IntPtr SubclassProc(IntPtr hwnd, uint message, nuint wparam, nint lparam, nuint id, nuint data);
        [StructLayout(LayoutKind.Sequential)] internal struct Point { internal int X, Y; }
        [StructLayout(LayoutKind.Sequential)] internal struct Rect { internal int Left, Top, Right, Bottom; }

        [DllImport("comctl32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc callback, nuint id, nuint data);
        [DllImport("comctl32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProc callback, nuint id);
        [DllImport("comctl32.dll")]
        internal static extern IntPtr DefSubclassProc(IntPtr hwnd, uint message, nuint wparam, nint lparam);
        [DllImport("user32.dll")]
        internal static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint process);
        [DllImport("kernel32.dll")]
        internal static extern uint GetCurrentThreadId();
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ScreenToClient(IntPtr hwnd, ref Point point);
        [DllImport("user32.dll")] internal static extern IntPtr SetFocus(IntPtr hwnd);
        [DllImport("user32.dll")] internal static extern IntPtr SetCapture(IntPtr hwnd);
        [DllImport("user32.dll")] internal static extern IntPtr GetCapture();
        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ReleaseCapture();
        [DllImport("user32.dll")] internal static extern short GetKeyState(int virtualKey);
    }
}
