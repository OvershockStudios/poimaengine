// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using Microsoft.Win32;

namespace Poima.Editor;

internal sealed record MsvcTools(string VisualCpp, string Compiler, string Bin);
internal sealed record WindowsSdk(string Root, string Version);

internal sealed record WindowsBuildToolchain(string? Dotnet, MsvcTools? Msvc, WindowsSdk? Sdk)
{
    public bool Ready => Dotnet is not null && Msvc is not null && Sdk is not null;
    public string Summary => Ready ? "C# gameplay builds are ready." : "Install " + string.Join(" and ",
        new[] { Dotnet is null ? "the x64 .NET 10 SDK" : null, Msvc is null || Sdk is null ? "C++ build tools with a Windows SDK" : null }
            .OfType<string>()) + " to build C# gameplay.";

    private static string VsInstallerDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
        "Microsoft Visual Studio", "Installer");

    public static string? VisualStudioInstaller
    {
        get
        {
            var executable = Path.Combine(VsInstallerDirectory, "vs_installer.exe");
            return File.Exists(executable) ? executable : null;
        }
    }

    public static string? WinGet
    {
        get
        {
            var executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Microsoft", "WindowsApps", "winget.exe");
            return File.Exists(executable) ? executable : null;
        }
    }

    [SupportedOSPlatform("windows")]
    public static async Task<WindowsBuildToolchain> DetectAsync()
    {
        var user = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var roots = new[] { Environment.GetEnvironmentVariable("DOTNET_ROOT_X64"), Environment.GetEnvironmentVariable("DOTNET_ROOT"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "dotnet"),
            Path.Combine(local, "Microsoft", "dotnet"), Path.Combine(user, ".dotnet") };
        var dotnet = FindDotnet(roots);
        var kits = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows Kits\Installed Roots", "KitsRoot10", null) as string;
        var sdk = FindWindowsSdk(kits);
        var vswhere = Path.Combine(VsInstallerDirectory, "vswhere.exe");
        var msvc = File.Exists(vswhere) ? await DiscoverVisualStudioAsync(vswhere).ConfigureAwait(false) : null;
        return new(dotnet, msvc, sdk);
    }

    internal static string? FindDotnet(IEnumerable<string?> roots)
    {
        foreach (var root in roots)
        {
            if (string.IsNullOrEmpty(root) || !Path.IsPathFullyQualified(root)) continue;
            var executable = Path.Combine(root, "dotnet.exe");
            var sdk = Path.Combine(root, "sdk");
            if (!File.Exists(executable) || !Directory.Exists(sdk) ||
                !Directory.EnumerateDirectories(sdk).Any(directory =>
                    Version.TryParse(Path.GetFileName(directory), out var version) && version.Major == 10 &&
                    File.Exists(Path.Combine(directory, "dotnet.dll")))) continue;
            try
            {
                using var image = new PEReader(File.OpenRead(executable));
                if (image.PEHeaders.CoffHeader.Machine == Machine.Amd64) return executable;
            }
            catch (BadImageFormatException) { }
        }
        return null;
    }

    private static async Task<MsvcTools?> DiscoverVisualStudioAsync(string locator)
    {
        using var process = Start(locator, "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-format", "json", "-utf8");
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        using var bound = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        try { await process.WaitForExitAsync(bound.Token).ConfigureAwait(false); }
        catch (OperationCanceledException)
        {
            try { if (!process.HasExited) process.Kill(); }
            catch (InvalidOperationException) when (process.HasExited) { }
            await process.WaitForExitAsync().ConfigureAwait(false);
            throw new TimeoutException("Visual Studio tool discovery timed out.");
        }
        await errors.ConfigureAwait(false);
        if (process.ExitCode != 0) throw new InvalidOperationException("Windows could not list Visual Studio installations.");
        return FindMsvc(await output.ConfigureAwait(false));
    }

    internal static MsvcTools? FindMsvc(string installations)
    {
        using var document = JsonDocument.Parse(installations);
        var candidates = document.RootElement.EnumerateArray().Select(instance =>
            (Root: instance.GetProperty("installationPath").GetString() ?? "",
            Version: Version.TryParse(instance.GetProperty("installationVersion").GetString(), out var version) ? version : null))
            .Where(instance => instance.Version is { Major: >= 17 } && Path.IsPathFullyQualified(instance.Root))
            .OrderByDescending(instance => instance.Version);
        foreach (var item in candidates)
        {
            var vc = Path.Combine(item.Root, "VC");
            var manifest = Path.Combine(vc, "Auxiliary", "Build", "Microsoft.VCToolsVersion.default.txt");
            if (!File.Exists(manifest) || !Version.TryParse(File.ReadAllText(manifest).Trim(), out var version)) continue;
            var compiler = Path.Combine(vc, "Tools", "MSVC", version.ToString());
            var bin = Path.Combine(compiler, "bin", "Hostx64", "x64");
            if (File.Exists(Path.Combine(bin, "link.exe")) && File.Exists(Path.Combine(bin, "cl.exe")) &&
                File.Exists(Path.Combine(compiler, "lib", "x64", "libcmt.lib")))
                return new(vc, compiler, bin);
        }
        return null;
    }

    internal static WindowsSdk? FindWindowsSdk(string? root)
    {
        if (root is null || !Path.IsPathFullyQualified(root)) return null;
        var lib = Path.Combine(root, "Lib");
        if (!Directory.Exists(lib)) return null;
        var versions = Directory.EnumerateDirectories(lib).Select(path => Path.GetFileName(path))
            .Select(name => (Name: name, Version: Version.TryParse(name, out var value) ? value : null))
            .Where(item => item.Version is { Major: 10 }).OrderByDescending(item => item.Version);
        foreach (var (name, _) in versions)
        {
            if (File.Exists(Path.Combine(lib, name, "um", "x64", "kernel32.lib")) &&
                File.Exists(Path.Combine(lib, name, "ucrt", "x64", "ucrt.lib")) &&
                File.Exists(Path.Combine(root, "Include", name, "um", "Windows.h")) &&
                File.Exists(Path.Combine(root, "Include", name, "ucrt", "corecrt.h")) &&
                File.Exists(Path.Combine(root, "bin", name, "x64", "rc.exe"))) return new(root, name);
        }
        return null;
    }

    private static Process Start(string executable, params string[] arguments)
    {
        var info = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return Process.Start(info) ?? throw new InvalidOperationException("Could not start Visual Studio tool detection.");
    }
}
