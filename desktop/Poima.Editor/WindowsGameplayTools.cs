// SPDX-License-Identifier: Apache-2.0
using System.Text;
using System.Text.Json;

namespace Poima.Editor;

internal static class WindowsGameplayTools
{
    internal const string ProfileName = "creator";
    private static string PackageDirectory => AppContext.BaseDirectory;
    private static string Authoring => Path.Combine(PackageDirectory, "authoring");
    public static bool HasAuthoringPack => File.Exists(Path.Combine(Authoring, "authoring.json"));

    public static async Task<LaunchOptions> ConfigureAsync(LaunchOptions options, ProjectLocation project,
        WindowsBuildToolchain? detected = null)
    {
        if (!OperatingSystem.IsWindows() || options.DevelopmentProfiles is not null || !HasAuthoringPack)
            return options;
        try
        {
            var tools = detected ?? await WindowsBuildToolchain.DetectAsync().ConfigureAwait(false);
            if (tools.Dotnet is not { } dotnet || tools.Msvc is not { } msvc || tools.Sdk is not { } sdk)
                return options with { GameplayToolStatus = tools.Summary };
            var python = Path.Combine(PackageDirectory, "publisher-python", "python.exe");
            if (!File.Exists(python)) throw new InvalidOperationException("Extract the complete Windows creator preview.");

            var parent = Path.GetDirectoryName(project.Root)
                ?? throw new InvalidOperationException("Move the project into its own folder before compiling gameplay.");
            var buildRoot = Path.Combine(parent, ".poima-build", project.ProjectId);
            var products = Directory.CreateDirectory(Path.Combine(buildRoot, "products")).FullName;
            var envi = BuildEnvironment(dotnet, msvc.VisualCpp, msvc.Compiler, msvc.Bin, sdk.Root, sdk.Version, buildRoot);
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

            var projectFile = Path.GetFileName(project.Manifest);
            var context = $$"""
                Poima configured profile `{{ProfileName}}` for `{{projectFile}}`. Agree on the game brief before editing.
                For build work, use `poima_call` → `world.reference` with guide `creator-gameplay` and the section that matches the task: `Write compiled C# gameplay`, `Publish and attach`, or `Check the playable build`.
                Inspect the world and starter source before editing. Use the profile to publish, run checks, and retain a passing build for Play. A guide name alone lists its headings.
                """;
            return options with { InstalledDevelopmentProfiles = profilePath, GameplayToolStatus = tools.Summary, GameplayToolContext = context };
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or JsonException or System.ComponentModel.Win32Exception or TimeoutException)
        {
            return options with { GameplayToolStatus = error.Message };
        }
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
