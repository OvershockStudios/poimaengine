// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text.Json;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed class GameplaySetupWindow : Window
{
    private readonly TextBlock dotnetState = RowStatus();
    private readonly TextBlock cppState = RowStatus();
    private readonly TextBlock message = RowStatus();
    private readonly Button dotnet = new() { Content = "Install .NET 10 SDK", IsVisible = false };
    private readonly Button cpp = new() { Content = "Install C++ build tools", IsVisible = false };
    private readonly Button check = new() { Content = "Check again" };
    private readonly Button done = new() { Content = "Done" };
    private readonly Button dotnetDownload = new() { Content = "Open .NET downloads", IsVisible = false };
    private readonly Button cppDownload = new() { Content = "Open C++ setup guide", IsVisible = false };
    private WindowsBuildToolchain? current;
    private bool busy;

    public GameplaySetupWindow()
    {
        Title = "Set up gameplay builds";
        Width = 575; MinWidth = 480; SizeToContent = SizeToContent.Height;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        done.Background = EditorTheme.Brush("#722F37");
        done.Foreground = EditorTheme.Brush("#F7E8EB");
        var body = new StackPanel { Margin = new Thickness(26), Spacing = 16 };
        body.Children.Add(new TextBlock { Text = "Gameplay build tools", FontSize = 24, FontWeight = FontWeight.SemiBold });
        body.Children.Add(new TextBlock { Text = "These Microsoft tools compile C# gameplay for Windows.",
            TextWrapping = TextWrapping.Wrap });
        body.Children.Add(ToolRow("x64 .NET 10 SDK", dotnetState, dotnet, dotnetDownload));
        body.Children.Add(ToolRow("C++ build tools", cppState, cpp, cppDownload));
        body.Children.Add(message);
        body.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 8, Children = { check, done } });
        Content = body;

        AutomationProperties.SetName(dotnetState, ".NET setup status");
        AutomationProperties.SetName(cppState, "C++ setup status");
        Opened += async (_, _) => await RunAsync(ProbeAsync);
        check.Click += async (_, _) => await RunAsync(ProbeAsync);
        dotnet.Click += async (_, _) => await RunAsync(() => InstallAsync(WindowsBuildDependency.Dotnet));
        cpp.Click += async (_, _) => await RunAsync(() => InstallAsync(WindowsBuildDependency.Cpp));
        dotnetDownload.Click += async (_, _) => await RunAsync(() => OpenGuideAsync(WindowsBuildDependency.Dotnet));
        cppDownload.Click += async (_, _) => await RunAsync(() => OpenGuideAsync(WindowsBuildDependency.Cpp));
        done.Click += (_, _) => Close(current?.Ready == true);
        Closing += (_, args) => args.Cancel = busy;
    }

    private static TextBlock RowStatus() => new()
    {
        TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#B4ACB2")
    };

    private static Control ToolRow(string title, TextBlock status, Button action, Button guide)
    {
        return new Border
        {
            BorderBrush = EditorTheme.Brush("#41383E"), BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8), Padding = new Thickness(14),
            Child = new StackPanel { Spacing = 8, Children =
                { new TextBlock { Text = title, FontWeight = FontWeight.SemiBold }, status,
                    new WrapPanel { ItemSpacing = 8, Children = { action, guide } } } }
        };
    }

    private async Task RunAsync(Func<Task> action)
    {
        if (busy) return;
        busy = true; UpdateButtons();
        try { await action(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException
            or System.ComponentModel.Win32Exception or JsonException or TimeoutException)
        {
            message.Text = error.Message;
            if (current is null) dotnetDownload.IsVisible = cppDownload.IsVisible = true;
        }
        finally { busy = false; UpdateButtons(); }
    }

    private async Task ProbeAsync()
    {
        if (!OperatingSystem.IsWindows()) return;
        message.Text = "Checking installed tools…";
        current = await Task.Run(WindowsBuildToolchain.DetectAsync);
        dotnetState.Text = current.Dotnet is null ? "Needed for C# builds." : "Ready";
        cppState.Text = (current.Msvc is not null, current.Sdk is not null) switch
        {
            (true, true) => $"Ready with Windows SDK {current.Sdk!.Version}",
            (true, false) => "Add an x64 Windows SDK in Visual Studio Installer.",
            (false, true) => "Add the desktop C++ build tools in Visual Studio Installer.",
            _ => "Needs the desktop C++ workload and an x64 Windows SDK."
        };
        var winget = WindowsBuildToolchain.WinGet;
        dotnet.Content = winget is null ? "Open .NET downloads" : "Install .NET 10 SDK";
        cpp.Content = WindowsBuildToolchain.VisualStudioInstaller is not null ? "Open Visual Studio Installer" :
            winget is not null ? "Install C++ build tools" : "Open C++ setup guide";
        dotnet.IsVisible = current.Dotnet is null;
        cpp.IsVisible = current.Msvc is null || current.Sdk is null;
        dotnetDownload.IsVisible = dotnet.IsVisible && winget is not null;
        cppDownload.IsVisible = cpp.IsVisible && (winget is not null || WindowsBuildToolchain.VisualStudioInstaller is not null);
        message.Text = current.Ready ? current.Summary : "Microsoft installers may ask for administrator approval.";
    }

    private async Task InstallAsync(WindowsBuildDependency dependency)
    {
        var start = WindowsBuildInstallation.Plan(dependency, WindowsBuildToolchain.WinGet, WindowsBuildToolchain.VisualStudioInstaller);
        using var process = Process.Start(start);
        if (start.UseShellExecute)
        {
            message.Text = "After setup finishes, return here and select Check again.";
            return;
        }
        if (process is null) throw new InvalidOperationException("Windows Package Manager could not be started.");
        message.Text = "Microsoft is installing the selected tools. Follow the installer window if it opens.";
        await process.WaitForExitAsync();
        await ProbeAsync();
        if (process.ExitCode != 0 && current?.Ready != true)
            message.Text = $"Installation returned 0x{unchecked((uint)process.ExitCode):X8}. Use the Microsoft setup link or try Check again.";
    }

    private Task OpenGuideAsync(WindowsBuildDependency dependency)
    {
        using var process = Process.Start(WindowsBuildInstallation.OpenDownloads(dependency));
        message.Text = "After setup finishes, return here and select Check again.";
        return Task.CompletedTask;
    }

    private void UpdateButtons()
    {
        check.IsEnabled = done.IsEnabled = dotnet.IsEnabled = cpp.IsEnabled =
            dotnetDownload.IsEnabled = cppDownload.IsEnabled = !busy;
    }
}
