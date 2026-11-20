// SPDX-License-Identifier: Apache-2.0
using System.Text;
using System.Text.Json;
using System.Runtime.Versioning;
using Microsoft.Win32;

namespace Poima.Editor;

internal static class WindowsGameplayTools
{
    private const string ProfileName = "creator";
    private static string PackageDirectory => AppContext.BaseDirectory;
    private static string Authoring => Path.Combine(PackageDirectory, "authoring");
    public static bool HasAuthoringPack => File.Exists(Path.Combine(Authoring, "authoring.json"));
    private static string DotnetSdkDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "dotnet");

    public static async Task<LaunchOptions> ConfigureAsync(LaunchOptions options, ProjectLocation project)
    {
        if (!OperatingSystem.IsWindows() || options.DevelopmentProfiles is not null || !HasAuthoringPack)
            return options;
        try
        {
            var dotnet = FindDotnet();
            var visualStudio = await FindVisualStudioAsync().ConfigureAwait(false);
            var vc = Path.Combine(visualStudio, "VC");
            var manifest = Path.Combine(vc, "Auxiliary", "Build", "Microsoft.VCToolsVersion.default.txt");
            if (!File.Exists(manifest) || !Version.TryParse(File.ReadAllText(manifest).Trim(), out var compilerVersion))
                throw new InvalidOperationException("Add the x64 MSVC tools in Visual Studio Installer.");
            var compiler = Path.Combine(vc, "Tools", "MSVC", compilerVersion.ToString());
            var compilerBin = Path.Combine(compiler, "bin", "Hostx64", "x64");
            if (!File.Exists(Path.Combine(compilerBin, "link.exe")) || !File.Exists(Path.Combine(compiler, "lib", "x64", "libcmt.lib")))
                throw new InvalidOperationException("Add the x64 MSVC tools in Visual Studio Installer.");
            var (sdk, sdkVersion) = FindWindowsSdk();
            var python = Path.Combine(PackageDirectory, "publisher-python", "python.exe");
            if (!File.Exists(python)) throw new InvalidOperationException("Extract the complete Windows creator preview.");

            var parent = Path.GetDirectoryName(project.Root)
                ?? throw new InvalidOperationException("Move the project into its own folder before compiling gameplay.");
            var buildRoot = Path.Combine(parent, ".poima-build", project.ProjectId);
            var products = Directory.CreateDirectory(Path.Combine(buildRoot, "products")).FullName;
            var envi = BuildEnvironment(dotnet, vc, compiler, compilerBin, sdk, sdkVersion, buildRoot);
            var profile = new
            {
                name = ProfileName, project_root = project.Root, output_root = products, python,
                publisher = Path.Combine(Authoring, "tools", "publish_native_gameplay.py"), dotnet,
                exporter = Path.Combine(ProjectFiles.PreviewRuntime, "bin", "poima.exe"),
                runtime_root = ProjectFiles.PreviewRuntime, authoring_pack = Authoring, target_rid = "win-x64",
                environment = envi.Select(pair => new[] { pair.Key, pair.Value }).ToArray()
            };
            var document = JsonSerializer.Serialize(new { format = "poima.development-profiles", version = 1, profiles = new[] { profile } });
            var profilePath = Path.Combine(buildRoot, "profiles.json");
            var temporary = Path.Combine(buildRoot, $"profiles-{Guid.NewGuid():N}.pending");
            try
            {
                await File.WriteAllTextAsync(temporary, document, new UTF8Encoding(false)).ConfigureAwait(false);
                File.Move(temporary, profilePath, overwrite: true);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }

            var guide = Path.Combine(PackageDirectory, "reference", "CREATOR_GAMEPLAY.md").Replace('\\', '/');
            var projectFile = Path.GetFileName(project.Manifest);
            var context = $"""
                Poima configured profile `{ProfileName}` for manifest `{projectFile}`. After brief approval, read `gameplay-src/Game.cs` with `poima_call` method `project.source.read` using profile, project, and path. To edit, submit the complete C# text and returned SHA-256 as `expected_sha256` with `poima_source` method `project.source.write`. The starter project is `gameplay-src/Game.csproj` with type `ProjectGameplay.Game`. Use `poima_build` for publication and adoption. See `{guide}` for C# API and verification details.
                """;
            return options with { InstalledDevelopmentProfiles = profilePath, GameplayToolStatus = "C# gameplay builds are ready.", GameplayToolContext = context };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception or TimeoutException)
        {
            return options with { GameplayToolStatus = error.Message };
        }
    }

    private static string FindDotnet()
    {
        var user = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var configured = Environment.GetEnvironmentVariable("DOTNET_ROOT");
        var roots = new[] { configured, DotnetSdkDirectory, Path.Combine(local, "Microsoft", "dotnet"), Path.Combine(user, ".dotnet") };
        foreach (var root in roots)
        {
            if (string.IsNullOrEmpty(root) || !Path.IsPathFullyQualified(root)) continue;
            var executable = Path.Combine(root, "dotnet.exe");
            var sdk = Path.Combine(root, "sdk");
            if (File.Exists(executable) && Directory.Exists(sdk) && Directory.EnumerateDirectories(sdk).Any(directory =>
                Version.TryParse(Path.GetFileName(directory), out var version) && version.Major == 10 &&
                File.Exists(Path.Combine(directory, "dotnet.dll")))) return executable;
        }
        throw new InvalidOperationException("Install the x64 .NET 10 SDK to build C# gameplay.");
    }

    private static async Task<string> FindVisualStudioAsync()
    {
        var locator = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            "Microsoft Visual Studio", "Installer", "vswhere.exe");
        const string install = "Install Visual Studio Build Tools or Visual Studio with Desktop development with C++.";
        if (!File.Exists(locator)) throw new InvalidOperationException(install);
        using var process = ProjectFiles.Start(locator, Encoding.UTF8, "-latest", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath", "-utf8");
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
        var location = (await output.ConfigureAwait(false)).Trim();
        await errors.ConfigureAwait(false);
        if (process.ExitCode != 0 || !Path.IsPathFullyQualified(location) || !Directory.Exists(location))
            throw new InvalidOperationException(install);
        return location;
    }

    [SupportedOSPlatform("windows")]
    private static (string Root, string Version) FindWindowsSdk()
    {
        var sdk = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows Kits\Installed Roots", "KitsRoot10", null) as string;
        var lib = sdk is null ? null : Path.Combine(sdk, "Lib");
        if (sdk is not null && Directory.Exists(lib))
        {
            var versions = Directory.EnumerateDirectories(lib!).Select(path => Path.GetFileName(path))
                .Select(name => (Name: name, Version: Version.TryParse(name, out var value) ? value : null))
                .Where(item => item.Version is { Major: 10 }).OrderByDescending(item => item.Version);
            foreach (var (name, _) in versions)
            {
                if (File.Exists(Path.Combine(sdk, "Lib", name, "um", "x64", "kernel32.lib")) &&
                    File.Exists(Path.Combine(sdk, "Lib", name, "ucrt", "x64", "ucrt.lib")) &&
                    File.Exists(Path.Combine(sdk, "Include", name, "um", "Windows.h")) &&
                    File.Exists(Path.Combine(sdk, "Include", name, "ucrt", "corecrt.h")) &&
                    File.Exists(Path.Combine(sdk, "bin", name, "x64", "rc.exe"))) return (sdk, name);
            }
        }
        throw new InvalidOperationException("Add an x64 Windows SDK in Visual Studio Installer.");
    }

    private static SortedDictionary<string, string> BuildEnvironment(string dotnet, string vc, string compiler,
        string compilerBin, string sdk, string sdkVersion, string buildRoot)
    {
        var env = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var name in new[] { "SystemRoot", "WINDIR", "SystemDrive", "COMSPEC", "OS", "USERPROFILE", "APPDATA",
                 "LOCALAPPDATA", "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432", "PROGRAMDATA", "PROCESSOR_ARCHITECTURE" })
            if (Environment.GetEnvironmentVariable(name) is { Length: > 0 } value) env[name] = value;
        var sdkBin = Path.Combine(sdk, "bin", sdkVersion, "x64");
        var includes = Path.Combine(sdk, "Include", sdkVersion);
        var libraries = Path.Combine(sdk, "Lib", sdkVersion);
        var vcLib = Path.Combine(compiler, "lib", "x64");
        env["PATH"] = string.Join(';', compilerBin, sdkBin, Path.GetDirectoryName(dotnet)!, Environment.SystemDirectory,
            Environment.GetFolderPath(Environment.SpecialFolder.Windows));
        env["INCLUDE"] = string.Join(';', new[] { Path.Combine(compiler, "include") }.Concat(
            new[] { "ucrt", "shared", "um", "winrt", "cppwinrt" }.Select(name => Path.Combine(includes, name)).Where(Directory.Exists)));
        env["LIB"] = string.Join(';', vcLib, Path.Combine(libraries, "ucrt", "x64"), Path.Combine(libraries, "um", "x64"));
        env["LIBPATH"] = vcLib;
        env["VCINSTALLDIR"] = Path.TrimEndingDirectorySeparator(vc) + "\\";
        env["VCToolsInstallDir"] = Path.TrimEndingDirectorySeparator(compiler) + "\\";
        env["WindowsSdkDir"] = Path.TrimEndingDirectorySeparator(sdk) + "\\";
        env["WindowsSDKVersion"] = sdkVersion + "\\";
        env["WindowsSdkVerBinPath"] = Path.Combine(sdk, "bin", sdkVersion) + "\\";
        env["UniversalCRTSdkDir"] = env["WindowsSdkDir"];
        env["UCRTVersion"] = sdkVersion;
        env["VSCMD_ARG_TGT_ARCH"] = env["VSCMD_ARG_HOST_ARCH"] = "x64";
        env["DOTNET_CLI_HOME"] = Directory.CreateDirectory(Path.Combine(buildRoot, "dotnet-home")).FullName;
        env["NUGET_PACKAGES"] = Environment.GetEnvironmentVariable("NUGET_PACKAGES") is { Length: > 0 } packages && Path.IsPathFullyQualified(packages)
            ? packages : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".nuget", "packages");
        // Native AOT linker response paths grow below the temporary directory.
        // Keep that work independent of the depth of the creator's project.
        env["TMP"] = env["TEMP"] = Path.GetTempPath();
        env["DOTNET_NOLOGO"] = "1";
        env["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1";
        return env;
    }
}
