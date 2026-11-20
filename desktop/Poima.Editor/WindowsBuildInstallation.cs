// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;

namespace Poima.Editor;

internal enum WindowsBuildDependency { Dotnet, Cpp }

internal static class WindowsBuildInstallation
{
    public static ProcessStartInfo OpenDownloads(WindowsBuildDependency dependency) => new(
        dependency == WindowsBuildDependency.Dotnet
            ? "https://dotnet.microsoft.com/download/dotnet/10.0"
            : "https://learn.microsoft.com/cpp/overview/acquire-msvc?view=msvc-170")
    {
        UseShellExecute = true
    };

    public static ProcessStartInfo Plan(WindowsBuildDependency dependency, string? winget, string? visualStudioInstaller)
    {
        if (dependency == WindowsBuildDependency.Cpp && visualStudioInstaller is not null)
            return new ProcessStartInfo(visualStudioInstaller) { UseShellExecute = true };
        if (winget is null) return OpenDownloads(dependency);

        var start = new ProcessStartInfo(winget) { UseShellExecute = false, CreateNoWindow = true };
        var package = dependency == WindowsBuildDependency.Dotnet ? "Microsoft.DotNet.SDK.10" : "Microsoft.VisualStudio.2022.BuildTools";
        string[] arguments = ["install", "--exact", "--id", package, "--source", "winget", "--architecture", "x64",
            "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity"];
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        if (dependency == WindowsBuildDependency.Cpp)
        {
            start.ArgumentList.Add("--override");
            start.ArgumentList.Add("--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools " +
                "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100");
        }
        return start;
    }
}
