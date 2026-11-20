// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Poima.AgentHost;

namespace Poima.Editor;

internal sealed class CodexSignInWindow : Window
{
    private readonly CodexSession codex;
    private readonly Button open = new() { Content = "Open sign-in page", IsEnabled = false };
    private readonly Button cancel = new() { Content = "Cancel" };
    private readonly TextBlock status = new() { Text = "Preparing sign-in…", TextWrapping = TextWrapping.Wrap };
    private readonly Dictionary<string, bool> earlyCompletions = new(StringComparer.Ordinal);
    private CodexBrowserLogin? attempt;
    private bool starting, cancelling, finished;
    private Task? cancelTask;

    public CodexSignInWindow(CodexSession codex)
    {
        this.codex = codex;
        Title = "Sign in to Codex";
        Width = 460;
        SizeToContent = SizeToContent.Height;
        CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        open.Background = EditorTheme.Brush("#722F37");
        open.Foreground = EditorTheme.Brush("#F7E8EB");
        AutomationProperties.SetName(status, "Codex sign-in status");
        Content = new StackPanel
        {
            Margin = new Thickness(24), Spacing = 14, Children =
            {
                new TextBlock { Text = "Continue with ChatGPT", FontSize = 22, FontWeight = FontWeight.SemiBold },
                status,
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { open, cancel } }
            }
        };
        Opened += async (_, _) => await StartAsync();
        open.Click += (_, _) => OpenBrowser();
        cancel.Click += async (_, _) => await CancelAsync();
        Closing += (_, args) =>
        {
            if (finished) return;
            args.Cancel = true;
            // Let the current close event return before closing from the async path.
            Avalonia.Threading.Dispatcher.UIThread.Post(async () => await CancelAsync());
        };
    }

    private async Task StartAsync()
    {
        starting = true;
        try
        {
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            var login = await codex.StartBrowserLoginAsync(deadline.Token);
            if (finished || cancelling) return;
            attempt = login;
            if (earlyCompletions.TryGetValue(login.Id, out var success)) Complete(success);
            else
            {
                status.Text = "Finish signing in in your browser. Poima will reconnect after Codex confirms sign-in.";
                open.IsEnabled = true;
            }
        }
        catch (Exception error) when (error is InvalidDataException or InvalidOperationException or AgentRpcException or AgentExchangeException)
        {
            if (!finished && !cancelling) status.Text = "Couldn’t start Codex sign-in. Close this window and connect to try again.";
        }
        finally { starting = false; earlyCompletions.Clear(); }
    }

    private void OpenBrowser()
    {
        if (attempt is not { } login || finished || cancelling) return;
        try { using var launched = Process.Start(new ProcessStartInfo(login.AuthorizationPage.AbsoluteUri) { UseShellExecute = true }); }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            status.Text = "Windows couldn’t open the sign-in page. Check your default browser and try again.";
        }
    }

    public void ObserveCompletion(JsonObject? parameters)
    {
        if (finished || cancelling || parameters?["loginId"] is not JsonValue value ||
            !value.TryGetValue<string>(out var id) || id.Length is < 1 or > 128 ||
            parameters["success"] is not JsonValue result || !result.TryGetValue<bool>(out var success)) return;
        if (attempt?.Id == id) Complete(success);
        else if (attempt is null && starting && earlyCompletions.Count < 8) earlyCompletions[id] = success;
    }

    private void Complete(bool success)
    {
        attempt = null;
        if (success) Finish(true);
        else
        {
            open.IsEnabled = false;
            status.Text = "Codex sign-in didn’t finish. Close this window and connect to try again.";
            cancel.Content = "Close";
        }
    }

    public Task CancelAsync() => cancelTask ??= CancelCoreAsync();

    private async Task CancelCoreAsync()
    {
        if (finished) return;
        cancelling = true;
        open.IsEnabled = cancel.IsEnabled = false;
        try
        {
            if (attempt is { } login)
            {
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                await codex.CancelBrowserLoginAsync(login.Id, deadline.Token);
            }
        }
        catch (Exception error) when (error is AgentRpcException or AgentExchangeException)
        {
            // Closing the creator connection also retires its app-server callback.
        }
        finally { Finish(false); }
    }

    private void Finish(bool success)
    {
        finished = true;
        Close(success);
    }
}
