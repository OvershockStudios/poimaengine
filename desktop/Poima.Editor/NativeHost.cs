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
    private readonly Dictionary<string, IntPtr> attached = new(StringComparer.Ordinal);
    private long requestId;
    private readonly HashSet<string> graphicsFailed = [];
    private readonly Dictionary<string, string?> viewErrors = new(StringComparer.Ordinal);
    private string? sessionError;
    private string? lastSelected;
    private string? notifiedError;
    public string WorldPath { get; }
    public string Endpoint { get; }
    public JsonObject State { get; private set; } = new();
    public string? LastError => sessionError ?? (ViewError("scene") is string scene ? "Scene: " + scene : ViewError("game") is string game ? "Game: " + game : null);
    public string? ViewError(string view) => viewErrors.GetValueOrDefault(view);
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
                || !JsonNode.DeepEquals(State["playback"], state["playback"])
                || !JsonNode.DeepEquals(State["gameplay"], state["gameplay"])
                || !JsonNode.DeepEquals(State["views"]?["game"]?["camera"], state["views"]?["game"]?["camera"])
                || !JsonNode.DeepEquals(State["input"], state["input"])
                || !JsonNode.DeepEquals(State["views"]?["game"]?["preparation_error"], state["views"]?["game"]?["preparation_error"]);
            State = state; lastSelected = selected; sessionError = null;
            foreach (var view in attached.Keys.ToArray())
            {
                if (graphicsFailed.Contains(view)) continue;
                var drawn = Native.DrawView(handle, view);
                if (drawn < 0)
                {
                    // Copy the error before another ABI call replaces its storage.
                    var drawError = Error();
                    graphicsFailed.Add(view);
                    try
                    {
                        var diagnostics = Call("desktop.inspect");
                        State = diagnostics;
                        // Snapshot preparation fails before the native renderer is
                        // touched. Retry those failures after authoring repairs;
                        // a poisoned renderer must still be detached/reattached.
                        if (diagnostics["views"]?[view] is JsonObject details && details.ContainsKey("graphics_error") && details["graphics_error"] is null) graphicsFailed.Remove(view);
                    }
                    catch (Exception inspectionError)
                    {
                        // An unknown native state is not safe to draw again.
                        drawError += " Renderer state unavailable: " + inspectionError.Message;
                    }
                    viewErrors[view] = drawError;
                }
                else if (drawn > 0 || view == "game" && State["views"]?["game"]?["camera"] is null) viewErrors[view] = null;
            }
            if (changed || notifiedError != LastError)
            {
                notifiedError = LastError;
                StateChanged?.Invoke(this, EventArgs.Empty);
            }
        }
        catch (Exception error)
        {
            sessionError = error.Message;
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
    internal void Attach(string view, IntPtr window)
    {
        Dispatcher.UIThread.VerifyAccess(); ObjectDisposedException.ThrowIf(handle == IntPtr.Zero, this);
        if (attached.Remove(view)) Native.DetachView(handle, view);
        if (Native.AttachView(handle, view, window) == 0) throw new InvalidOperationException(Error());
        attached[view] = window; graphicsFailed.Remove(view); viewErrors[view] = null;
    }
    internal void Detach(string view, IntPtr window)
    {
        Dispatcher.UIThread.VerifyAccess();
        if (handle != IntPtr.Zero && attached.GetValueOrDefault(view) == window)
        { Native.DetachView(handle, view); attached.Remove(view); graphicsFailed.Remove(view); viewErrors.Remove(view); }
    }
    public void Dispose()
    {
        Dispatcher.UIThread.VerifyAccess();
        if (handle == IntPtr.Zero) return;
        foreach (var view in attached.Keys) Native.DetachView(handle, view);
        Native.Destroy(handle); handle = IntPtr.Zero; attached.Clear();
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
        [DllImport(Library, EntryPoint = "poima_desktop_attach_view", CallingConvention = CallingConvention.Cdecl)] internal static extern int AttachView(IntPtr host, [MarshalAs(UnmanagedType.LPUTF8Str)] string view, IntPtr window);
        [DllImport(Library, EntryPoint = "poima_desktop_draw_view", CallingConvention = CallingConvention.Cdecl)] internal static extern int DrawView(IntPtr host, [MarshalAs(UnmanagedType.LPUTF8Str)] string view);
        [DllImport(Library, EntryPoint = "poima_desktop_detach_view", CallingConvention = CallingConvention.Cdecl)] internal static extern void DetachView(IntPtr host, [MarshalAs(UnmanagedType.LPUTF8Str)] string view);
        [DllImport(Library, EntryPoint = "poima_desktop_destroy", CallingConvention = CallingConvention.Cdecl)] internal static extern void Destroy(IntPtr host);
    }
}

public interface IViewportInteraction
{
    void Handle(ViewportInputEvent value);
    void Attach(ViewportInput input, IntPtr window);
    void Detach(ViewportInput input);
}

public sealed class VulkanView(NativeHost host, string view, IViewportInteraction interaction) : NativeControlHost
{
    private ViewportInput? input;
    protected override IPlatformHandle CreateNativeControlCore(IPlatformHandle parent)
    {
        var child = base.CreateNativeControlCore(parent);
        try
        {
            host.Attach(view, child.Handle);
            input = new ViewportInput(child.Handle, interaction.Handle, qualificationInput: Program.Options.Script is not null);
            interaction.Attach(input, child.Handle); return child;
        }
        catch { input?.Dispose(); input = null; host.Detach(view, child.Handle); base.DestroyNativeControlCore(child); throw; }
    }
    protected override void DestroyNativeControlCore(IPlatformHandle control)
    {
        try
        {
            if (input is not null)
            {
                var previous = input; input = null;
                try { interaction.Detach(previous); }
                finally { previous.Dispose(); }
            }
        }
        finally
        {
            try { host.Detach(view, control.Handle); }
            finally { base.DestroyNativeControlCore(control); }
        }
    }
}
