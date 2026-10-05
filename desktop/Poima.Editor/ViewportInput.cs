// SPDX-License-Identifier: Apache-2.0
using System.ComponentModel;
using System.Runtime.InteropServices;
using Avalonia.Threading;

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
    private readonly bool qualificationInput;
    private int qualificationDispatchDepth;
    private int pointerDownDepth;
    private bool rawRegistered;
    private Native.RawDevice? previousRawMouse;
    private bool cursorClipped;
    private Native.Rect previousClip, ownedClip, capturedScreenBounds;
    private IntPtr previousCursor;
    public bool GameCapture { get; private set; }
    public long RelativePackets { get; private set; }
    public long IgnoredAbsolutePackets { get; private set; }

    /// <summary>Call synchronously from an explicit viewport PointerDown. Script
    /// capture is modeled only: it never registers raw input, clips or hides the cursor.</summary>
    public void BeginGameCapture()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (GameCapture) return;
        if (window == IntPtr.Zero || pointerDownDepth == 0 || Width == 0 || Height == 0)
            throw new InvalidOperationException("Game capture requires an explicit viewport click.");
        if (qualificationInput)
        {
            if (qualificationDispatchDepth == 0) throw new InvalidOperationException("Script capture requires scoped input.");
            GameCapture = true;
            return;
        }
        if (Native.GetFocus() != window || Native.GetForegroundWindow() != Native.GetAncestor(window, 2))
            throw new InvalidOperationException("Game capture requires the focused foreground viewport.");
        try
        {
            previousRawMouse = RegisteredMouse();
            RegisterMouse(new Native.RawDevice { UsagePage = 1, Usage = 2, Target = window });
            rawRegistered = true;
            if (!Native.GetClipCursor(out previousClip)) throw new Win32Exception(Marshal.GetLastWin32Error());
            var top = new Native.Point(); var bottom = new Native.Point { X = Width, Y = Height };
            if (!Native.ClientToScreen(window, ref top) || !Native.ClientToScreen(window, ref bottom))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            // Respect any pre-existing restriction; never expand another owner's clip.
            capturedScreenBounds = new Native.Rect { Left = top.X, Top = top.Y, Right = bottom.X, Bottom = bottom.Y };
            ownedClip = new Native.Rect { Left = Math.Max(previousClip.Left, top.X), Top = Math.Max(previousClip.Top, top.Y),
                Right = Math.Min(previousClip.Right, bottom.X), Bottom = Math.Min(previousClip.Bottom, bottom.Y) };
            if (ownedClip.Right <= ownedClip.Left || ownedClip.Bottom <= ownedClip.Top)
                throw new InvalidOperationException("The viewport is outside the available cursor region.");
            if (!Native.ClipCursor(ref ownedClip)) throw new Win32Exception(Marshal.GetLastWin32Error());
            cursorClipped = true;
            Native.SetCapture(window);
            if (Native.GetCapture() != window) throw new InvalidOperationException("Game pointer capture failed.");
            previousCursor = Native.SetCursor(IntPtr.Zero);
            GameCapture = true;
        }
        catch { CleanupGameCapture(); ReleaseOwnedCapture(); throw; }
    }

    /// <summary>Call each UI pump, including while the Game panel is hidden.
    /// Parent/dock moves do not necessarily produce messages on the child HWND.</summary>
    public void ValidateGameCapture()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (!GameCapture || qualificationInput) return;
        var root = Native.GetAncestor(window, 2);
        bool valid = window != IntPtr.Zero && Native.GetFocus() == window && Native.GetCapture() == window
            && Native.GetForegroundWindow() == root && Native.IsWindowVisible(window) && !Native.IsIconic(root);
        if (valid)
        {
            var top = new Native.Point(); var bottom = new Native.Point { X = Width, Y = Height };
            valid = Native.ClientToScreen(window, ref top) && Native.ClientToScreen(window, ref bottom)
                && capturedScreenBounds.Left == top.X && capturedScreenBounds.Top == top.Y
                && capturedScreenBounds.Right == bottom.X && capturedScreenBounds.Bottom == bottom.Y;
        }
        if (!valid) Reset(ViewportInputKind.CaptureLost, true);
    }

    public void EndGameCapture()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (GameCapture || rawRegistered || cursorClipped) Reset(ViewportInputKind.CaptureLost, true);
    }

    internal static void DispatchQualificationRelative(IntPtr hwnd, int dx, int dy)
    {
        if (Math.Abs((long)dx) > 32767 || Math.Abs((long)dy) > 32767)
            throw new ArgumentOutOfRangeException(nameof(dx), "Script relative motion is bounded to 32767 units per axis.");
        DispatchQualification(hwnd, () =>
        {
            var input = Active.Values.Single(value => value.window == hwnd);
            if (!input.GameCapture) throw new InvalidOperationException("Script relative motion requires Game capture.");
            ++input.RelativePackets;
            input.Emit(new(ViewportInputKind.RelativeMotion, DeltaX: dx, DeltaY: dy));
        });
    }

    private static Native.RawDevice? RegisteredMouse()
    {
        uint count = 0; uint size = (uint)Marshal.SizeOf<Native.RawDevice>();
        if (Native.GetRegisteredRawInputDevices(null, ref count, size) == uint.MaxValue)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (count > 128) throw new InvalidOperationException("Too many process raw input registrations.");
        if (count == 0) return null;
        var devices = new Native.RawDevice[count];
        uint read = Native.GetRegisteredRawInputDevices(devices, ref count, size);
        if (read == uint.MaxValue) throw new Win32Exception(Marshal.GetLastWin32Error());
        for (int i = 0; i < read; ++i)
            if (devices[i].UsagePage == 1 && devices[i].Usage == 2) return devices[i];
        return null;
    }

    private static void RegisterMouse(Native.RawDevice device)
    {
        if (!Native.RegisterRawInputDevices([device], 1, (uint)Marshal.SizeOf<Native.RawDevice>()))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    private void CleanupGameCapture()
    {
        GameCapture = false;
        // Cleanup each resource independently; never strand clipping on a raw-input error.
        if (rawRegistered)
        {
            rawRegistered = false;
            try
            {
                var current = RegisteredMouse();
                if (current is { } owned && owned.Target == window && owned.Flags == 0)
                    RegisterMouse(previousRawMouse ?? new Native.RawDevice { UsagePage = 1, Usage = 2, Flags = 1 }); // RIDEV_REMOVE
            }
            catch (Exception error)
            {
                LastError = "Raw input restore: " + error.Message;
                // A former SDL receiver may have been destroyed meanwhile. Remove
                // only our still-current registration if restoring it failed.
                try
                {
                    var current = RegisteredMouse();
                    if (current is { } owned && owned.Target == window && owned.Flags == 0)
                        RegisterMouse(new Native.RawDevice { UsagePage = 1, Usage = 2, Flags = 1 });
                }
                catch (Exception cleanup) { LastError += " Raw input removal: " + cleanup.Message; }
            }
            previousRawMouse = null;
        }
        if (cursorClipped)
        {
            cursorClipped = false;
            if (Native.GetClipCursor(out var current) && current.Equals(ownedClip) && !Native.ClipCursor(ref previousClip))
                LastError = "Cursor clipping restore failed.";
        }
        if (previousCursor != IntPtr.Zero)
        {
            if (Native.GetCursor() == IntPtr.Zero) Native.SetCursor(previousCursor);
            previousCursor = IntPtr.Zero;
        }
    }

    private void RawMotion(nuint wparam, nint lparam)
    {
        // RIM_INPUT=0 only; never process background INPUTSINK packets.
        if (!GameCapture || qualificationInput || wparam != 0) return;
        ValidateGameCapture();
        if (!GameCapture) return;
        uint size = 0; uint header = (uint)(8 + 2 * IntPtr.Size);
        if (Native.GetRawInputData(lparam, 0x10000003, IntPtr.Zero, ref size, header) == uint.MaxValue)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (size < header + 24 || size > 4096) return;
        var memory = Marshal.AllocHGlobal((int)size);
        try
        {
            uint read = Native.GetRawInputData(lparam, 0x10000003, memory, ref size, header);
            if (read == uint.MaxValue) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (read < header + 24 || Marshal.ReadInt32(memory) != 0) return; // RIM_TYPEMOUSE
            int offset = (int)header;
            if ((Marshal.ReadInt16(memory, offset) & 1) != 0) { ++IgnoredAbsolutePackets; return; }
            int dx = Marshal.ReadInt32(memory, offset + 12), dy = Marshal.ReadInt32(memory, offset + 16);
            ++RelativePackets;
            if (dx != 0 || dy != 0) Emit(new(ViewportInputKind.RelativeMotion, DeltaX: dx, DeltaY: dy));
        }
        finally { Marshal.FreeHGlobal(memory); }
    }

    public int Width { get; private set; }
    public int Height { get; private set; }
    public bool QualificationInput => qualificationInput;
    public long IgnoredInteractiveMessages { get; private set; }
    /// <summary>Latest caught callback/Win32 error; exceptions never escape WindowProc.</summary>
    public string? LastError { get; private set; }

    public ViewportInput(IntPtr hwnd, Action<ViewportInputEvent> onEvent, bool qualificationInput = false)
    {
        Dispatcher.UIThread.VerifyAccess();
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Viewport HWND input requires Windows.");
        ArgumentNullException.ThrowIfNull(onEvent);
        var thread = Native.GetWindowThreadProcessId(hwnd, out var process);
        if (hwnd == IntPtr.Zero || thread == 0 || thread != Native.GetCurrentThreadId() || process != (uint)Environment.ProcessId)
            throw new ArgumentException("The viewport must be an owned window on the current UI thread.", nameof(hwnd));
        this.onEvent = onEvent;
        this.qualificationInput = qualificationInput;
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

    internal static void DispatchQualification(IntPtr hwnd, Action dispatch)
    {
        Dispatcher.UIThread.VerifyAccess();
        var input = Active.Values.SingleOrDefault(value => value.window == hwnd)
            ?? throw new InvalidOperationException("Qualification viewport is no longer attached.");
        if (!input.qualificationInput) throw new InvalidOperationException("Scoped input requires an explicit qualification script.");
        ++input.qualificationDispatchDepth;
        try { dispatch(); }
        finally { --input.qualificationDispatchDepth; }
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
                else
                {
                    input.Process(message, wparam, lparam);
                    if (message == 0x0020 && input.GameCapture && !input.qualificationInput)
                    { Native.SetCursor(IntPtr.Zero); return (IntPtr)1; }
                }
            }
        }
        catch (Exception error)
        {
            if (Active.TryGetValue(subclassId, out var input)) input.Fail(error);
        }
        // Preserve SDL/default processing, especially WM_INPUT foreground cleanup.
        // Only the captured cursor image (WM_SETCURSOR above) is owned here.
        return Native.DefSubclassProc(hwnd, message, wparam, lparam);
    }

    private void Process(uint message, nuint wparam, nint lparam)
    {
        // Explicit --script qualification accepts interactive messages only
        // during its synchronous SendMessage call. Windows can generate mouse
        // movement after capture even without scripted movement. Ignore it
        // before it changes pointer/button state or acquires focus/capture.
        // Real lifecycle events always pass: loss of capture/focus, resize and
        // destruction must still cancel a gesture or expose a failed test.
        if (qualificationInput && qualificationDispatchDepth == 0 &&
            (message is >= 0x0200 and <= 0x020E || message is 0x0100 or 0x0101 or 0x0104 or 0x0105 or 0x00FF))
        {
            ++IgnoredInteractiveMessages;
            return;
        }
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
                ++pointerDownDepth;
                try { Emit(new(ViewportInputKind.PointerDown, Button: button,
                    ClickCount: message is 0x0203 or 0x0206 or 0x0209 or 0x020D ? 2 : 1)); }
                finally { --pointerDownDepth; }
                break;
            }
            case 0x0202: case 0x0205: case 0x0208: case 0x020C:
            {
                ReadPoint(lparam);
                var button = MouseButton(message, wparam);
                if (button == ViewportMouseButton.None) return;
                buttons &= ~Mask(button);
                Emit(new(ViewportInputKind.PointerUp, Button: button));
                if (buttons == ViewportMouseButtons.None && !GameCapture) ReleaseOwnedCapture();
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
                if (GameCapture && wparam is 0x1B or 0x09) EndGameCapture();
                Emit(KeyEvent(ViewportInputKind.KeyDown, wparam, lparam));
                break;
            case 0x0101: case 0x0105:
                Emit(KeyEvent(ViewportInputKind.KeyUp, wparam, lparam));
                break;
            case 0x00FF: // WM_INPUT; always forwarded for foreground packet cleanup.
                RawMotion(wparam, lparam);
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
            case 0x0003: // WM_MOVE (parent moves additionally checked by ValidateGameCapture).
                if (GameCapture) Reset(ViewportInputKind.CaptureLost, true);
                break;
            case 0x0018: // WM_SHOWWINDOW
                if (GameCapture && wparam == 0) Reset(ViewportInputKind.CaptureLost, true);
                break;
            case 0x0005: case 0x02E3: // WM_SIZE / WM_DPICHANGED_AFTERPARENT
                if (GameCapture) Reset(ViewportInputKind.CaptureLost, true);
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
            CleanupGameCapture();
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
                Modifiers = ReadModifiers(), Width = Width, Height = Height,
                Synthetic = qualificationInput && qualificationDispatchDepth > 0 });
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
        try { CleanupGameCapture(); ReleaseOwnedCapture(); }
        catch (Exception cleanupError) { LastError += "\nCapture cleanup: " + cleanupError.Message; }
        finally { suppressCaptureNotification = previous; }
    }

    private void RefreshSize()
    {
        if (!Native.GetClientRect(window, out var rect)) throw new Win32Exception(Marshal.GetLastWin32Error());
        Width = Math.Max(0, rect.Right - rect.Left);
        Height = Math.Max(0, rect.Bottom - rect.Top);
    }

    private static ViewportInputEvent KeyEvent(ViewportInputKind kind, nuint virtualKey, nint bits)
    {
        uint make = (uint)(((long)bits >> 16) & 255);
        bool extended = (((long)bits >> 24) & 1) != 0;
        uint scan = PhysicalScanCode(make, extended, (uint)virtualKey);
        return new(kind, VirtualKey: (uint)virtualKey, Repeat: kind == ViewportInputKind.KeyDown && (((long)bits >> 30) & 1) != 0,
            ScanCode: scan, NativeScanCode: make, ExtendedKey: extended);
    }

    // USB usages match SDL_Scancode / Poima input.describe. Unsupported OEM keys
    // remain 0: guessing a layout-dependent VK would silently break physical binds.
    private static uint PhysicalScanCode(uint code, bool extended, uint vk)
    {
        if (vk == 0x90) return 83; // Num Lock commonly sets the extended bit.
        if (vk == 0x13) return 72; // Pause's E1 sequence is collapsed by WM_KEY*.
        if (vk == 0x2C) return 70; // Print Screen can arrive with a synthetic make code.
        if (code == 0) return 0;
        if (extended) return code switch
        {
            0x1C => 88, 0x1D => 228, 0x35 => 84, 0x38 => 230,
            0x47 => 74, 0x48 => 82, 0x49 => 75, 0x4B => 80, 0x4D => 79,
            0x4F => 77, 0x50 => 81, 0x51 => 78, 0x52 => 73, 0x53 => 76,
            0x5B => 227, 0x5C => 231, 0x5D => 101, _ => 0
        };
        return code switch
        {
            0x01 => 41, >= 0x02 and <= 0x0A => code + 28, 0x0B => 39,
            0x0C => 45, 0x0D => 46, 0x0E => 42, 0x0F => 43,
            0x10 => 20, 0x11 => 26, 0x12 => 8, 0x13 => 21, 0x14 => 23,
            0x15 => 28, 0x16 => 24, 0x17 => 12, 0x18 => 18, 0x19 => 19,
            0x1A => 47, 0x1B => 48, 0x1C => 40, 0x1D => 224,
            0x1E => 4, 0x1F => 22, 0x20 => 7, 0x21 => 9, 0x22 => 10,
            0x23 => 11, 0x24 => 13, 0x25 => 14, 0x26 => 15, 0x27 => 51,
            0x28 => 52, 0x29 => 53, 0x2A => 225, 0x2B => 49,
            0x2C => 29, 0x2D => 27, 0x2E => 6, 0x2F => 25, 0x30 => 5,
            0x31 => 17, 0x32 => 16, 0x33 => 54, 0x34 => 55, 0x35 => 56,
            0x36 => 229, 0x37 => 85, 0x38 => 226, 0x39 => 44, 0x3A => 57,
            >= 0x3B and <= 0x44 => code - 1, 0x45 => 83, 0x46 => 71,
            0x47 => 95, 0x48 => 96, 0x49 => 97, 0x4A => 86,
            0x4B => 92, 0x4C => 93, 0x4D => 94, 0x4E => 87,
            0x4F => 89, 0x50 => 90, 0x51 => 91, 0x52 => 98, 0x53 => 99,
            0x56 => 100, 0x57 => 68, 0x58 => 69, 0x59 => 103,
            >= 0x64 and <= 0x6E => code + 4, 0x76 => 115, _ => 0
        };
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

        [StructLayout(LayoutKind.Sequential)] internal struct RawDevice
        { internal ushort UsagePage, Usage; internal uint Flags; internal IntPtr Target; }
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool RegisterRawInputDevices([In] RawDevice[] devices, uint count, uint size);
        [DllImport("user32.dll", SetLastError = true)]
        internal static extern uint GetRegisteredRawInputDevices([Out] RawDevice[]? devices, ref uint count, uint size);
        [DllImport("user32.dll", SetLastError = true)]
        internal static extern uint GetRawInputData(nint input, uint command, IntPtr data, ref uint size, uint headerSize);
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ClientToScreen(IntPtr hwnd, ref Point point);
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ClipCursor(ref Rect rect);
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetClipCursor(out Rect rect);
        [DllImport("user32.dll")] internal static extern IntPtr SetCursor(IntPtr cursor);
        [DllImport("user32.dll")] internal static extern IntPtr GetCursor();
        [DllImport("user32.dll")] internal static extern IntPtr GetFocus();
        [DllImport("user32.dll")] internal static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool IsIconic(IntPtr hwnd);
        [DllImport("user32.dll")] internal static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

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
