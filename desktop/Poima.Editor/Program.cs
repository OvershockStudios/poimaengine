// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using System.Text.Json.Nodes;

namespace Poima.Editor;

internal sealed record LaunchOptions(string World, string Endpoint, int Gpu, uint Samples, int Frames, string? Report, bool SoftwareUi, string? Script, string? Layout)
{
    public static LaunchOptions Parse(string[] args)
    {
        if (args.Length == 0 || args[0].StartsWith("--"))
            throw new ArgumentException("Use: Poima.Editor <world.json> [--endpoint name] [--gpu N] [--samples 1|4] [--frames N] [--report new.json] [--script actions.json] [--software-ui] [--layout preferences.json]");
        var world = Path.GetFullPath(args[0]); var endpoint = "poima-desktop"; var gpu = -1; uint samples = 4; var frames = 0;
        string? report = null, script = null, layout = null; var software = false; var seen = new HashSet<string>();
        for (var i = 1; i < args.Length; i++)
        {
            var key = args[i]; if (!seen.Add(key)) throw new ArgumentException("Repeated option: " + key);
            if (key == "--software-ui") { software = true; continue; }
            if (++i >= args.Length) throw new ArgumentException("Missing value: " + key);
            var value = args[i];
            switch (key)
            {
                case "--endpoint": endpoint = value; break;
                case "--gpu": gpu = int.Parse(value); if (gpu is < 0 or > 4095) throw new ArgumentException("GPU must be 0..4095."); break;
                case "--samples": samples = uint.Parse(value); if (samples is not (1 or 4)) throw new ArgumentException("Samples must be 1 or 4."); break;
                case "--frames": frames = int.Parse(value); if (frames is < 1 or > 36000) throw new ArgumentException("Frames must be 1..36000."); break;
                case "--report": report = Path.GetFullPath(value); break;
                case "--script": script = Path.GetFullPath(value); break;
                case "--layout": layout = Path.GetFullPath(value); break;
                default: throw new ArgumentException("Unknown option: " + key);
            }
        }
        if (!Directory.Exists(Path.GetDirectoryName(world))) throw new ArgumentException("World parent directory must exist.");
        if (endpoint.Length is < 1 or > 64 || endpoint.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '_' && c != '-'))
            throw new ArgumentException("Endpoint must contain 1..64 ASCII letters, digits, '_' or '-'.");
        if (report != null)
        {
            if (File.Exists(report) || Directory.Exists(report) || !Directory.Exists(Path.GetDirectoryName(report)))
                throw new ArgumentException("Report requires a new file in an existing directory.");
            var parent = Path.GetDirectoryName(world)! + Path.DirectorySeparatorChar;
            if (report.StartsWith(parent, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Report must be outside the project directory.");
        }
        if (script != null && (frames == 0 || !File.Exists(script)))
            throw new ArgumentException("A qualification script needs an existing file and an explicit --frames limit.");
        return new(world, endpoint, gpu, samples, frames, report, software, script, layout);
    }
}

internal static class Program
{
    public static LaunchOptions Options { get; private set; } = null!;
    [STAThread]
    public static int Main(string[] args)
    {
        try
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("This desktop prototype currently targets Windows.");
            Options = LaunchOptions.Parse(args);
            return AppBuilder.Configure<App>().UsePlatformDetect()
                .With(new Win32PlatformOptions { RenderingMode = [Options.SoftwareUi ? Win32RenderingMode.Software : Win32RenderingMode.Vulkan] })
                .LogToTrace().StartWithClassicDesktopLifetime(args);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            if (Options?.Report is string path)
                try { using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                    using var writer = new StreamWriter(stream); writer.Write(new JsonObject { ["success"] = false, ["error"] = error.ToString() }.ToJsonString()); }
                catch (IOException) { }
            return 4;
        }
    }
}
