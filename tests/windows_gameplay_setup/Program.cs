// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using Poima.Editor;

if (!OperatingSystem.IsWindows() || args.Length != 1)
    throw new ArgumentException("Run on Windows with a new fixture directory.");
var root = Directory.CreateDirectory(args[0]).FullName;
Require(!Directory.EnumerateFileSystemEntries(root).Any(), "Fixture directory must be empty.");

var actual = await WindowsBuildToolchain.DetectAsync();
Require(actual.Ready, "Installed Microsoft toolchain is available");
Require(actual.Summary == "C# gameplay builds are ready.", "Ready status");

Require(WindowsBuildToolchain.FindDotnet([Path.Combine(root, "missing-dotnet"), "sdk"]) is null &&
    WindowsBuildToolchain.FindMsvc("[]") is null && WindowsBuildToolchain.FindWindowsSdk(Path.Combine(root, "missing-sdk")) is null,
    "Missing prerequisites are reported");
var missing = new WindowsBuildToolchain(null, null, null);
Require(!missing.Ready && missing.Summary.Contains(".NET 10 SDK") && missing.Summary.Contains("C++ build tools"), "Combined missing status");

var invalidDotnet = Path.Combine(root, "not a compiler");
Write(invalidDotnet, "dotnet.exe", "Not a PE executable");
Write(invalidDotnet, "sdk/10.0.400/dotnet.dll", "Fixture");
var supportedDotnet = Path.Combine(root, "supported .NET");
Directory.CreateDirectory(supportedDotnet);
File.Copy(actual.Dotnet!, Path.Combine(supportedDotnet, "dotnet.exe"));
Write(supportedDotnet, "sdk/9.0.300/dotnet.dll", "Fixture");
Require(WindowsBuildToolchain.FindDotnet([invalidDotnet, supportedDotnet]) is null, "Invalid image and wrong SDK age rejected");
Write(supportedDotnet, "sdk/10.0.400/dotnet.dll", "Fixture");
Require(WindowsBuildToolchain.FindDotnet([invalidDotnet, supportedDotnet]) == Path.Combine(supportedDotnet, "dotnet.exe"),
    "x64 SDK discovered after invalid candidate");

var fullVs = Path.Combine(root, "full VS");
var incompleteVs = Path.Combine(root, "incomplete VS");
foreach (var candidate in new[] { fullVs, incompleteVs })
{
    Write(candidate, "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt", "14.42.0");
    Write(candidate, "VC/Tools/MSVC/14.42.0/bin/Hostx64/x64/link.exe", "Fixture");
    Write(candidate, "VC/Tools/MSVC/14.42.0/bin/Hostx64/x64/cl.exe", "Fixture");
}
Write(fullVs, "VC/Tools/MSVC/14.42.0/lib/x64/libcmt.lib", "Fixture");
var versions = JsonSerializer.Serialize(new[]
{
    new { installationPath = incompleteVs, installationVersion = "18.0.0" },
    new { installationPath = fullVs, installationVersion = "17.14.0" }
});
Require(WindowsBuildToolchain.FindMsvc(versions)?.VisualCpp == Path.Combine(fullVs, "VC"),
    "Complete C++ toolset selected from older Visual Studio installation");

var kits = Path.Combine(root, "Kits");
foreach (var version in new[] { "10.0.28000.0", "10.0.26100.0" })
{
    Write(kits, $"Lib/{version}/um/x64/kernel32.lib", "Fixture");
    Write(kits, $"Lib/{version}/ucrt/x64/ucrt.lib", "Fixture");
    Write(kits, $"Include/{version}/um/Windows.h", "Fixture");
    Write(kits, $"Include/{version}/ucrt/corecrt.h", "Fixture");
}
Write(kits, "bin/10.0.26100.0/x64/rc.exe", "Fixture");
Require(WindowsBuildToolchain.FindWindowsSdk(kits)?.Version == "10.0.26100.0", "Complete x64 SDK selected");

var winGet = Path.Combine(root, "winget.exe");
var vsInstaller = Path.Combine(root, "VS Installer", "vs_installer.exe");
var dotnetPlan = WindowsBuildInstallation.Plan(WindowsBuildDependency.Dotnet, winGet, vsInstaller);
Require(!dotnetPlan.UseShellExecute && dotnetPlan.FileName == winGet &&
    dotnetPlan.ArgumentList.Contains("Microsoft.DotNet.SDK.10") && dotnetPlan.ArgumentList.Contains("x64"),
    "WinGet .NET setup selects exact native SDK");
var cppPlan = WindowsBuildInstallation.Plan(WindowsBuildDependency.Cpp, winGet, null);
Require(!cppPlan.UseShellExecute && cppPlan.ArgumentList.Contains("Microsoft.VisualStudio.2022.BuildTools") &&
    cppPlan.ArgumentList[^1].Contains("--norestart") &&
    cppPlan.ArgumentList[^1].Contains("Microsoft.VisualStudio.Component.Windows11SDK.26100"),
    "WinGet Build Tools setup requests MSVC, SDK and no reboot");
var modify = WindowsBuildInstallation.Plan(WindowsBuildDependency.Cpp, winGet, vsInstaller);
Require(modify.UseShellExecute && modify.FileName == vsInstaller && modify.ArgumentList.Count == 0,
    "Existing Visual Studio installation is managed in the vendor UI");
foreach (var dependency in Enum.GetValues<WindowsBuildDependency>())
{
    var fallback = WindowsBuildInstallation.Plan(dependency, null, null);
    Require(fallback.UseShellExecute && new Uri(fallback.FileName).Host is "dotnet.microsoft.com" or "learn.microsoft.com",
        "Missing WinGet uses the official browser fallback");
}

Console.WriteLine(JsonSerializer.Serialize(new
{
    passed = true, real_windows_prerequisites_ready = actual.Ready, windows_sdk = actual.Sdk!.Version,
    winget_available = WindowsBuildToolchain.WinGet is not null,
    microsoft_installers_started = 0,
    cases = new[] { "fully_missing_prerequisites", "invalid_dotnet_pe_and_older_sdk", "complete_msvc_fallback",
        "complete_windows_sdk_fallback", "exact_winget_packages", "existing_visual_studio_route", "vendor_download_fallback" }
}));

static void Write(string root, string relative, string text)
{
    var path = Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar));
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllText(path, text);
}

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
