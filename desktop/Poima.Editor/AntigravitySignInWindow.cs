// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Threading;
using Poima.AgentHost;

namespace Poima.Editor;

internal sealed class AntigravitySignInWindow : Window
{
    private const string Personal = "oauth-personal";
    private const string Enterprise = "oauth-business";
    private readonly CancellationTokenSource lifetime = new();
    private readonly TextBlock status = new() { Text = "Choose an account to continue in your browser.", TextWrapping = TextWrapping.Wrap };
    private readonly Button google = new() { Content = "Sign in with Google" };
    private readonly Button enterprise = new() { Content = "Gemini Enterprise" };
    private readonly Button cancel = new() { Content = "Cancel" };
    private bool finished;

    public static bool Available(AcpSession agent) =>
        agent.OffersAuthentication(Personal) || agent.OffersAuthentication(Enterprise);

    public AntigravitySignInWindow(AcpSession agent)
    {
        Title = "Sign in to Antigravity";
        Width = 490;
        SizeToContent = SizeToContent.Height;
        CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        google.IsVisible = agent.OffersAuthentication(Personal);
        enterprise.IsVisible = agent.OffersAuthentication(Enterprise);
        google.Background = EditorTheme.Brush("#722F37");
        google.Foreground = EditorTheme.Brush("#F7E8EB");
        AutomationProperties.SetName(status, "Antigravity sign-in status");
        Content = new StackPanel
        {
            Margin = new Thickness(24), Spacing = 14, Children =
            {
                new TextBlock { Text = "Sign in to Antigravity", FontSize = 22, FontWeight = FontWeight.SemiBold },
                status,
                new WrapPanel { ItemSpacing = 8, LineSpacing = 8, Children = { google, enterprise, cancel } }
            }
        };
        google.Click += async (_, _) => await SignInAsync(agent, Personal);
        enterprise.Click += async (_, _) => await SignInAsync(agent, Enterprise);
        cancel.Click += (_, _) => Cancel();
        Closing += (_, args) =>
        {
            if (finished) return;
            args.Cancel = true;
            Dispatcher.UIThread.Post(Cancel);
        };
        Closed += (_, _) => lifetime.Dispose();
    }

    public void Cancel()
    {
        if (finished) return;
        lifetime.Cancel();
        Finish(false);
    }

    private async Task SignInAsync(AcpSession agent, string method)
    {
        if (finished || !google.IsEnabled) return;
        google.IsEnabled = enterprise.IsEnabled = false;
        status.Text = "Finish signing in in your browser. You can cancel here.";
        try
        {
            await agent.AuthenticateAsync(method, lifetime.Token);
            if (!finished) Finish(true);
        }
        catch (Exception error) when (error is AgentRpcException or AgentExchangeException or OperationCanceledException or IOException)
        {
            if (finished) return;
            status.Text = "Antigravity sign-in didn’t finish. Close this window and connect to try again.";
            cancel.Content = "Close";
        }
    }

    private void Finish(bool success)
    {
        finished = true;
        Close(success);
    }
}
