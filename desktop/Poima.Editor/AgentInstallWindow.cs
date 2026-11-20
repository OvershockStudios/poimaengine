// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed class AgentInstallWindow : Window
{
    private readonly CancellationTokenSource cancellation = new(TimeSpan.FromMinutes(10));
    private readonly TextBlock status = new() { Text = "Preparing the download…", TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar progress = new() { Minimum = 0, Maximum = 100, Height = 6, IsIndeterminate = true };
    private readonly Button cancel = new() { Content = "Cancel" };
    private readonly Button guide = new() { Content = "Open setup guide", IsVisible = false };
    private bool installing = true;
    private readonly WindowsAgentPackage package;

    public AgentInstallWindow(WindowsAgentPackage package, string details, string guideUrl)
    {
        this.package = package;
        Title = "Install " + package.Name;
        Width = 480;
        SizeToContent = SizeToContent.Height;
        CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        AutomationProperties.SetName(status, package.Name + " install status");
        Content = new StackPanel
        {
            Margin = new Thickness(24), Spacing = 14, Children =
            {
                new TextBlock { Text = $"{package.Name} {package.Version}", FontSize = 22, FontWeight = FontWeight.SemiBold },
                new TextBlock { Text = details, TextWrapping = TextWrapping.Wrap },
                progress, status,
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { cancel, guide } }
            }
        };
        Opened += async (_, _) => await InstallAsync();
        cancel.Click += (_, _) => Stop();
        guide.Click += (_, _) =>
        {
            try { OpenGuide(guideUrl); }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
            { status.Text = $"Windows couldn’t open the {package.Name} setup guide."; }
        };
        Closing += (_, args) =>
        {
            if (!installing) return;
            args.Cancel = true;
            RequestCancel();
        };
        Closed += (_, _) => cancellation.Dispose();
    }

    public static void OpenGuide(string url)
    {
        using var launched = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
    }

    public void Stop()
    {
        if (installing) RequestCancel();
        else Close(null);
    }

    private void RequestCancel()
    {
        cancellation.Cancel();
        cancel.IsEnabled = false;
        status.Text = "Stopping installation…";
    }

    private async Task InstallAsync()
    {
        try
        {
            using var http = new HttpClient(new SocketsHttpHandler { ConnectTimeout = TimeSpan.FromSeconds(20) })
                { Timeout = Timeout.InfiniteTimeSpan };
            var updates = new Progress<int>(percent =>
            {
                if (!installing || cancellation.IsCancellationRequested) return;
                progress.IsIndeterminate = percent == 100;
                progress.Value = percent;
                status.Text = percent == 100 ? $"Verifying and installing {package.Name}…" : $"Downloading {package.Name}… {percent}%";
            });
            var installed = await package.InstallAsync(http, package.Root, updates, cancellation.Token);
            installing = false;
            Close(cancel.IsEnabled ? installed : null);
        }
        catch (OperationCanceledException) when (!cancel.IsEnabled)
        {
            installing = false;
            Close(null);
        }
        catch (Exception error) when (error is HttpRequestException or IOException or InvalidDataException
            or UnauthorizedAccessException or OperationCanceledException)
        {
            installing = false;
            progress.IsVisible = false;
            status.Text = error is OperationCanceledException ? $"The {package.Name} download timed out." : error.Message;
            guide.IsVisible = cancel.IsEnabled = true;
            cancel.Content = "Close";
        }
    }
}
