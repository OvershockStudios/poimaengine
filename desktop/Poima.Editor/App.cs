// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Themes.Simple;
using Avalonia.Threading;
using Dock.Avalonia.Themes.Simple;
using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Reflection;
using Avalonia.Platform;
using Avalonia.Media;

namespace Poima.Editor;

public sealed class App : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        Styles.Add(new SimpleTheme()); Styles.Add(new DockSimpleTheme());
    }
    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var options = Program.Options;
            var script = new DesktopScript(options.Script, options.Frames);
            var host = new NativeHost(options.World, options.Endpoint, options.Gpu, options.Samples);
            MainWindow window;
            try { window = new MainWindow(host, options.Layout); }
            catch { host.Dispose(); throw; }
            desktop.MainWindow = window; desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
            var frames = 0; var watch = Stopwatch.StartNew();
            var renderScaling = window.RenderScaling;
            var previousTime = watch.Elapsed.TotalSeconds;
            timer.Tick += (_, _) =>
            {
                renderScaling = window.RenderScaling;
                window.Game.ValidateCapture();
                host.Pump();
                var now = watch.Elapsed.TotalSeconds; window.Navigation.Tick(now-previousTime); window.Game.Tick(); previousTime = now;
                script.Tick(frames, window); ++frames;
                if (script.Error != null || (options.Frames > 0 && frames >= options.Frames)) window.CloseQualification();
            };
            window.Opened += (_, _) => timer.Start();
            desktop.Exit += (_, exit) =>
            {
                timer.Stop();
                try
                {
                    var state = host.Call("desktop.inspect");
                    var failure = script.Error ?? (script.Completed ? host.LastError : "Qualification ended before all scripted actions completed.");
                    if (failure != null) exit.ApplicationExitCode = 4;
                    var report = new JsonObject { ["success"] = failure == null, ["error"] = failure,
                        ["frontend"] = "Avalonia 12.1.3 / Dock 12.1.0.6", ["ui_backend_requested"] = options.SoftwareUi ? "software" : "Vulkan only",
                        ["ui_backend_actual"] = PlatformGraphicsType(),
                        ["font_resolved"] = FontManager.Current.TryGetGlyphTypeface(new Typeface(window.FontFamily), out var glyphs) ? glyphs.FamilyName : null,
                        ["render_scaling"] = renderScaling, ["base_font_dip"] = window.FontSize,
                        ["elapsed_ms"] = watch.Elapsed.TotalMilliseconds, ["pump_frames"] = frames,
                        ["private_bytes"] = Process.GetCurrentProcess().PrivateMemorySize64, ["state"] = state,
                        ["world"] = options.World, ["endpoint"] = options.Endpoint,
                        ["navigation"] = window.Navigation.Inspect(), ["game_input"] = window.Game.Inspect(), ["layout_file"] = window.LayoutPath, ["layout_error"] = window.LayoutError,
                        ["actions"] = script.Results.DeepClone(), ["draft"] = window.InspectDraft(),
                        ["limitations"] = new JsonArray("Prototype: physical IME/accessibility and full Unity parity are not qualified.") };
                    if (options.Report != null)
                    {
                        using var stream = new FileStream(options.Report, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                        using var writer = new StreamWriter(stream); writer.Write(report.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                    }
                    Console.WriteLine(report.ToJsonString());
                }
                catch (Exception error)
                {
                    exit.ApplicationExitCode = 4;
                    Console.Error.WriteLine(error);
                }
                finally { host.Dispose(); }
            };
        }
        base.OnFrameworkInitializationCompleted();
    }
    private static string? PlatformGraphicsType()
    {
        // Diagnostic only: Avalonia's locator is not part of our engine protocol.
        var locator = typeof(AvaloniaObject).Assembly.GetType("Avalonia.AvaloniaLocator");
        var resolver = locator?.GetProperty("Current", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)?.GetValue(null);
        return resolver?.GetType().GetMethod("GetService", [typeof(Type)])?.Invoke(resolver, [typeof(IPlatformGraphics)])?.GetType().FullName;
    }
}
