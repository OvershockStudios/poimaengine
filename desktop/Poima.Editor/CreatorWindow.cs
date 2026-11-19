// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace Poima.Editor;

internal sealed class CreatorWindow : Window
{
    private readonly ProjectLocation project;
    private readonly NativeHost host;
    private readonly EditorModel model;
    private readonly GameInput game;
    private readonly AgentChat chat;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    private readonly Button play = new() { Content = "Play" }, stop = new() { Content = "Stop" };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Border viewport = new() { Background = EditorTheme.Brush("#161518"), ClipToBounds = true };
    private bool starting, closing, allowClose;
    private Task? agentShutdown;
    private string? previewSession, error;

    public CreatorWindow(ProjectLocation project, LaunchOptions options)
    {
        this.project = project;
        Title = project.Name + " — Poima Creator";
        Width = 1180; Height = 760; MinWidth = 760; MinHeight = 560;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        UseLayoutRounding = true;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        host = new NativeHost(project.World, options.Endpoint, options.Gpu, options.Samples);
        try
        {
            model = new EditorModel(host);
            game = new GameInput(model);
            game.Error += message => { error = message; UpdateControls(); };
            chat = new AgentChat(project.Root, host.Endpoint, creator: true);
            Content = BuildContent();
        }
        catch { host.Dispose(); throw; }
        timer.Tick += (_, _) => Pump();
        Opened += (_, _) => { UpdateControls(); timer.Start(); };
        play.Click += async (_, _) => await PlayAsync();
        stop.Click += (_, _) => ExecuteStop();
        Closing += (_, args) =>
        {
            if (allowClose) return;
            args.Cancel = true;
            if (closing) return;
            try
            {
                game.Release();
                host.Call("desktop.close.begin");
                closing = true;
                host.RefreshState();
                viewport.Child = null;
                agentShutdown = chat.CloseAsync();
            }
            catch (Exception failure) { error = failure.Message; }
            UpdateControls();
        };
        Closed += (_, _) =>
        {
            timer.Stop();
            model.Dispose();
            host.Dispose();
        };
    }

    private Control BuildContent()
    {
        var title = new StackPanel { Spacing = 4 };
        title.Children.Add(new TextBlock { Text = project.Name, FontSize = 24, FontWeight = FontWeight.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        title.Children.Add(new TextBlock { Text = project.Root, FontSize = 12, Foreground = EditorTheme.Brush("#B4ACB2"),
            TextTrimming = TextTrimming.CharacterEllipsis });
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), Margin = new Thickness(24), RowSpacing = 20 };
        root.Children.Add(title);
        var panels = new Grid { ColumnDefinitions = new ColumnDefinitions("5*,7*"), ColumnSpacing = 16 };
        panels.Children.Add(Card(chat));
        var preview = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 12, Margin = new Thickness(16) };
        var bar = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        bar.Children.Add(new TextBlock { Text = "Game preview", FontSize = 20,
            FontWeight = FontWeight.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        var transport = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        play.Background = EditorTheme.Brush("#722F37");
        play.Foreground = EditorTheme.Brush("#F7E8EB");
        AutomationProperties.SetName(play, "Play project");
        AutomationProperties.SetName(stop, "Stop preview");
        transport.Children.Add(play); transport.Children.Add(stop);
        Grid.SetColumn(transport, 1); bar.Children.Add(transport);
        preview.Children.Add(bar);
        Grid.SetRow(viewport, 1); preview.Children.Add(viewport);
        AutomationProperties.SetName(status, "Preview status");
        Grid.SetRow(status, 2); preview.Children.Add(status);
        var previewCard = Card(preview);
        Grid.SetColumn(previewCard, 1); panels.Children.Add(previewCard);
        Grid.SetRow(panels, 1); root.Children.Add(panels);
        return root;
    }

    private static Border Card(Control child) => new() { Child = child, Background = EditorTheme.Brush("#232125"),
        BorderBrush = EditorTheme.Brush("#41383E"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8) };

    private async Task PlayAsync()
    {
        if (starting || closing || chat.Busy) return;
        starting = true; error = null; UpdateControls();
        string? started = null;
        try
        {
            // Resolve the current manifest through the CLI before starting a
            // playtest. The native guarded startup pins the inspected revision.
            var current = await ProjectFiles.Open(project.Manifest);
            if (closing) return;
            if (chat.Busy) throw new InvalidOperationException("Wait for Codex to finish its turn, then try Play again.");
            if (current.World != project.World || current.Endpoint != project.Endpoint)
                throw new InvalidOperationException("The project entry changed. Reopen the project to play it.");
            host.RefreshState();
            if (model.RuntimeId is not null)
                throw new InvalidOperationException("Stop the current playtest before starting a new preview.");
            model.SetGameCamera(current.Camera);
            game.SelectProfile(current.InputProfile);
            var configured = host.Call("desktop.gameplay.inspect");
            var session = EditorModel.NewId();
            host.Call("desktop.play.start", new() { ["session_id"] = session, ["revision"] = current.Revision,
                ["paused"] = true, ["expected_gameplay_generation"] = configured["generation"]!.DeepClone() });
            started = session;
            if (current.Gameplay is { } module && configured["profile"] is null)
            {
                var descriptor = Path.GetFullPath(module.GetProperty("descriptor").GetString()!, project.Root);
                host.Call("runtime.gameplay.load_native", new() { ["session_id"] = session, ["request_id"] = EditorModel.NewId(),
                    ["expected_tick"] = 0, ["expected_revision"] = 0, ["descriptor"] = descriptor,
                    ["values"] = JsonNode.Parse(module.GetProperty("values").GetRawText()) });
            }
            var audio = host.Call("desktop.audio.inspect");
            if (audio["config"]?["enabled"]?.GetValue<bool>() != current.Audio)
            {
                host.Call("desktop.audio.configure", new() { ["request_id"] = EditorModel.NewId(),
                    ["expected_generation"] = audio["generation"]!.DeepClone(), ["enabled"] = current.Audio,
                    ["muted"] = audio["config"]!["muted"]!.DeepClone(), ["volume"] = audio["config"]!["volume"]!.DeepClone() });
            }
            host.Call("desktop.play.resume", new() { ["session_id"] = session });
            host.RefreshState();
            viewport.Child = new VulkanView(host, "game", game);
            previewSession = session;
        }
        catch (Exception failure)
        {
            error = failure.Message;
            viewport.Child = null;
            if (started is not null)
            {
                try { StopRuntime(started); }
                catch (Exception cleanup) { error += " Unable to stop the preview. " + cleanup.Message; }
            }
        }
        finally { starting = false; UpdateControls(); }
    }

    private void ExecuteStop()
    {
        if (closing || starting || chat.Busy) return;
        error = null;
        try
        {
            host.RefreshState();
            if (model.RuntimeId is string session) StopRuntime(session);
            viewport.Child = null; previewSession = null;
        }
        catch (Exception failure) { error = failure.Message; }
        UpdateControls();
    }

    private void StopRuntime(string session)
    {
        game.Release();
        host.Call("desktop.play.stop", new() { ["session_id"] = session });
        host.RefreshState();
    }

    private void Pump()
    {
        if (!closing) game.ValidateCapture();
        host.Pump();
        if (closing)
        {
            if (agentShutdown?.IsCompleted == true && host.State["audio"]?["closed"]?.GetValue<bool>() == true)
            {
                allowClose = true;
                Close();
            }
            return;
        }
        if (previewSession is not null && model.RuntimeId != previewSession)
        {
            viewport.Child = null; previewSession = null;
        }
        if (previewSession is not null) game.Tick();
        UpdateControls();
    }

    private void UpdateControls()
    {
        play.IsEnabled = !closing && !starting && !chat.Busy && model.RuntimeId is null;
        stop.IsEnabled = !closing && !starting && !chat.Busy && model.RuntimeId is not null;
        var gameView = host.State["views"]?["game"];
        status.Text = closing ? "Closing project…" : starting ? "Starting preview…" : error ?? host.ViewError("game") ??
            model.PlaybackError ?? (previewSession is not null
                ? gameView?["presented_tick"] is not null ? "Click the game to take control. Press Esc to release the mouse." : "Waiting for the first game frame…"
                : model.RuntimeId is not null ? "A playtest is running. Stop it to start a preview."
                : "Play opens the current project. Chat changes appear the next time you play.");
    }
}
