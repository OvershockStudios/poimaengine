// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Avalonia.Controls;
using Avalonia.Platform;
using Avalonia.Threading;

namespace Poima.Editor;

public sealed class NativeHost : IDisposable
{
    private IntPtr handle;
    private IntPtr attached;
    private long requestId;
    private bool graphicsFailed;
    private string? lastSelected;
    private string? notifiedError;
    public string WorldPath { get; }
    public string Endpoint { get; }
    public JsonObject State { get; private set; } = new();
    public string? LastError { get; private set; }
    public event EventHandler? StateChanged;

    public NativeHost(string world, string endpoint, int gpu, uint samples)
    {
        Dispatcher.UIThread.VerifyAccess();
        WorldPath = Path.GetFullPath(world); Endpoint = endpoint;
        handle = Native.Create(WorldPath, endpoint, gpu, samples);
        if (handle == IntPtr.Zero) throw new InvalidOperationException(Error());
        State = Read(Native.Poll(handle));
    }
    private string Error() => Marshal.PtrToStringUTF8(Native.Error(handle)) ?? "Native desktop bridge failed.";
    private JsonObject Read(IntPtr pointer)
    {
        if (pointer == IntPtr.Zero) throw new InvalidOperationException(Error());
        return JsonNode.Parse(Marshal.PtrToStringUTF8(pointer)!)?.AsObject()
            ?? throw new InvalidOperationException("Native bridge returned an empty response.");
    }
    public JsonObject Call(string method, JsonObject? parameters = null)
    {
        Dispatcher.UIThread.VerifyAccess(); ObjectDisposedException.ThrowIf(handle == IntPtr.Zero, this);
        var request = new JsonObject { ["jsonrpc"] = "2.0", ["id"] = ++requestId, ["method"] = method,
            ["params"] = parameters?.DeepClone() ?? new JsonObject() };
        var reply = Read(Native.Call(handle, request.ToJsonString()));
        if (reply["error"] is JsonObject error)
            throw new InvalidOperationException($"{error["message"]} ({error["code"]})");
        return reply["result"]?.DeepClone().AsObject() ?? new JsonObject();
    }
    public void Pump()
    {
        Dispatcher.UIThread.VerifyAccess(); if (handle == IntPtr.Zero) return;
        try
        {
            var state = Read(Native.Poll(handle));
            var selected = state["selected"]?.ToString();
            var changed = state["world_changed"]?.GetValue<bool>() == true || state["runtime_changed"]?.GetValue<bool>() == true || selected != lastSelected
                || !JsonNode.DeepEquals(State["playback"], state["playback"]) || !JsonNode.DeepEquals(State["view"], state["view"])
                || !JsonNode.DeepEquals(State["input"], state["input"]);
            State = state; lastSelected = selected;
            if (attached != IntPtr.Zero && !graphicsFailed)
            {
                var drawn = Native.Draw(handle);
                if (drawn < 0)
                {
                    // Copy the error before another ABI call replaces its storage.
                    var drawError = Error();
                    graphicsFailed = true;
                    try
                    {
                        var diagnostics = Call("desktop.inspect");
                        State = diagnostics;
                        // Snapshot preparation fails before the native renderer is
                        // touched. Retry those failures after authoring repairs;
                        // a poisoned renderer must still be detached/reattached.
                        graphicsFailed = !diagnostics.ContainsKey("graphics_error") || diagnostics["graphics_error"] is not null;
                    }
                    catch (Exception inspectionError)
                    {
                        // An unknown native state is not safe to draw again.
                        drawError += " Renderer state unavailable: " + inspectionError.Message;
                    }
                    LastError = drawError;
                }
                else if (drawn > 0) LastError = null;
            }
            if (changed || notifiedError != LastError)
            {
                notifiedError = LastError;
                StateChanged?.Invoke(this, EventArgs.Empty);
            }
        }
        catch (Exception error)
        {
            LastError = error.Message;
            if (notifiedError != LastError)
            { notifiedError = LastError; StateChanged?.Invoke(this, EventArgs.Empty); }
        }
    }
    public void RefreshState()
    {
        // Inspect is read-only and never advances the native playback clock.
        State = Call("desktop.inspect"); lastSelected = State["selected"]?.ToString();
        StateChanged?.Invoke(this, EventArgs.Empty);
    }
    internal void Attach(IntPtr window)
    {
        Dispatcher.UIThread.VerifyAccess(); ObjectDisposedException.ThrowIf(handle == IntPtr.Zero, this);
        if (attached != IntPtr.Zero) Native.Detach(handle);
        attached = IntPtr.Zero;
        if (Native.Attach(handle, window) == 0) throw new InvalidOperationException(Error());
        attached = window; graphicsFailed = false; LastError = null;
    }
    internal void Detach(IntPtr window)
    {
        Dispatcher.UIThread.VerifyAccess();
        if (handle != IntPtr.Zero && attached == window) { Native.Detach(handle); attached = IntPtr.Zero; }
    }
    public void Dispose()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (handle == IntPtr.Zero) return;
        Native.Detach(handle); Native.Destroy(handle); handle = IntPtr.Zero; attached = IntPtr.Zero;
        GC.SuppressFinalize(this);
    }
    private static class Native
    {
        private const string Library = "poima_desktop";
        [DllImport(Library, EntryPoint = "poima_desktop_create", CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr Create([MarshalAs(UnmanagedType.LPUTF8Str)] string world, [MarshalAs(UnmanagedType.LPUTF8Str)] string endpoint, int gpu, uint samples);
        [DllImport(Library, EntryPoint = "poima_desktop_call", CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr Call(IntPtr host, [MarshalAs(UnmanagedType.LPUTF8Str)] string json);
        [DllImport(Library, EntryPoint = "poima_desktop_poll", CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr Poll(IntPtr host);
        [DllImport(Library, EntryPoint = "poima_desktop_error", CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr Error(IntPtr host);
        [DllImport(Library, EntryPoint = "poima_desktop_attach", CallingConvention = CallingConvention.Cdecl)] internal static extern int Attach(IntPtr host, IntPtr window);
        [DllImport(Library, EntryPoint = "poima_desktop_draw", CallingConvention = CallingConvention.Cdecl)] internal static extern int Draw(IntPtr host);
        [DllImport(Library, EntryPoint = "poima_desktop_detach", CallingConvention = CallingConvention.Cdecl)] internal static extern void Detach(IntPtr host);
        [DllImport(Library, EntryPoint = "poima_desktop_destroy", CallingConvention = CallingConvention.Cdecl)] internal static extern void Destroy(IntPtr host);
    }
}

public sealed class VulkanView(NativeHost host, SceneNavigation navigation) : NativeControlHost
{
    private ViewportInput? input;
    protected override IPlatformHandle CreateNativeControlCore(IPlatformHandle parent)
    {
        var child = base.CreateNativeControlCore(parent);
        try
        {
            host.Attach(child.Handle);
            input = new ViewportInput(child.Handle, navigation.Handle, qualificationInput: Program.Options.Script is not null);
            navigation.Attach(input, child.Handle); return child;
        }
        catch { input?.Dispose(); input = null; host.Detach(child.Handle); base.DestroyNativeControlCore(child); throw; }
    }
    protected override void DestroyNativeControlCore(IPlatformHandle control)
    {
        try
        {
            if (input is not null)
            {
                var previous = input; input = null;
                try { navigation.Detach(previous); }
                finally { previous.Dispose(); }
            }
        }
        finally
        {
            try { host.Detach(control.Handle); }
            finally { base.DestroyNativeControlCore(control); }
        }
    }
}
