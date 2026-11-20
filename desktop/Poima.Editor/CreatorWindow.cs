// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
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
    private readonly Button export = new() { Content = "Export build" };
    private readonly Button showExport = new() { Content = "Open build folder", IsVisible = false };
    private readonly Button showChecks = new() { Content = "Open check report", IsVisible = false };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Border viewport = new() { Background = EditorTheme.Brush("#161518"), ClipToBounds = true };
    private bool starting, exporting, closeAfterExport, closing, allowClose;
    private string? exportedFolder, checkReport, checkSummary;
    private long exportedRevision;
    private Task? agentShutdown;
    private CancellationTokenSource? checking;
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
        host = new NativeHost(project.World, options.Endpoint, options.Gpu, options.Samples, options.DevelopmentProfiles);
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
        export.Click += async (_, _) => await ExportAsync();
        showExport.Click += (_, _) => OpenExportItem(exportedFolder, Directory.Exists);
        showChecks.Click += (_, _) => OpenExportItem(checkReport, File.Exists);
        Closing += (_, args) =>
        {
            if (allowClose) return;
            args.Cancel = true;
            if (closing) return;
            if (exporting)
            {
                closeAfterExport = true;
                checking?.Cancel();
                UpdateControls();
                return;
            }
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
        AutomationProperties.SetName(export, "Export Windows build");
        export.IsVisible = File.Exists(Path.Combine(ProjectFiles.PreviewRuntime, "runtime.json"));
        transport.Children.Add(play); transport.Children.Add(stop);
        transport.Children.Add(export);
        Grid.SetColumn(transport, 1); bar.Children.Add(transport);
        preview.Children.Add(bar);
        Grid.SetRow(viewport, 1); preview.Children.Add(viewport);
        AutomationProperties.SetName(status, "Preview status");
        var result = new StackPanel { Spacing = 8 };
        result.Children.Add(status);
        result.Children.Add(showExport);
        result.Children.Add(showChecks);
        Grid.SetRow(result, 2); preview.Children.Add(result);
        var previewCard = Card(preview);
        Grid.SetColumn(previewCard, 1); panels.Children.Add(previewCard);
        Grid.SetRow(panels, 1); root.Children.Add(panels);
        return root;
    }

    private static Border Card(Control child) => new() { Child = child, Background = EditorTheme.Brush("#232125"),
        BorderBrush = EditorTheme.Brush("#41383E"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8) };

    private async Task PlayAsync()
    {
        if (starting || exporting || closing || chat.Busy) return;
        starting = true; error = null; UpdateControls();
        string? started = null;
        try
        {
            // Resolve the current manifest through the CLI before starting a
            // playtest. The native guarded startup pins the inspected revision.
            var current = await OpenCurrentProject();
            if (closing) return;
            if (chat.Busy) throw new InvalidOperationException("Wait for the agent to finish its turn, then try Play again.");
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
        if (closing || starting || exporting || chat.Busy) return;
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

    private async Task<ProjectLocation> OpenCurrentProject()
    {
        var current = await ProjectFiles.Open(project.Manifest);
        if (current.World != project.World || current.Endpoint != project.Endpoint)
            throw new InvalidOperationException("The project entry changed. Reopen the project to continue.");
        return current;
    }

    private async Task ExportAsync()
    {
        if (!export.IsEnabled || !export.IsVisible) return;
        exporting = true; error = null; checkReport = null; UpdateControls();
        try
        {
            var current = await OpenCurrentProject();
            host.RefreshState();
            if (model.RuntimeId is not null || chat.Busy)
                throw new InvalidOperationException("Stop the playtest and wait for the agent before exporting.");
            if (host.Call("desktop.gameplay.inspect")["profile"] is not null)
                throw new InvalidOperationException("Clear the desktop gameplay profile and attach the published Native AOT game before exporting.");

            var parent = Path.GetDirectoryName(current.Root)
                ?? throw new InvalidOperationException("Move the project into its own folder before exporting.");
            var name = Path.GetFileName(current.Root);
            if (name.Length > 160) name = "Poima";
            var suffix = Guid.NewGuid().ToString("N")[..8];
            var folder = Path.Combine(parent, $"{name} - Build {DateTime.Now:yyyy-MM-dd HHmmss} {suffix}");
            var result = await ProjectFiles.Command("build", current.Manifest, "--output", folder,
                "--runtime", ProjectFiles.PreviewRuntime);
            var candidate = Path.GetFullPath(result.GetProperty("output").GetString()!);
            var revision = result.GetProperty("revision").GetInt64();
            var suite = Path.Combine(current.Root, ProjectFiles.VerificationSuiteName);
            if (closeAfterExport) return;
            if (File.Exists(suite))
            {
                var report = candidate + ".checks.json";
                using var cancellation = new CancellationTokenSource();
                checking = cancellation;
                UpdateControls();
                try
                {
                    var verification = await ProjectFiles.VerifyExport(candidate, suite, report, cancellation.Token);
                    var passed = verification.GetProperty("groups_passed").GetInt32();
                    var total = verification.GetProperty("groups_total").GetInt32();
                    checkReport = report;
                    if (!verification.GetProperty("passed").GetBoolean())
                    {
                        error = $"Revision {revision} passed {passed} of {total} check groups." + EarlierExportMessage();
                        return;
                    }
                    checkSummary = $"Revision {revision} passed all {total} check groups.";
                }
                finally { checking = null; }
            }
            else if (checkSummary is not null)
            {
                error = $"Revision {revision} was exported without a check suite." + EarlierExportMessage();
                return;
            }
            exportedFolder = candidate;
            exportedRevision = revision;
        }
        catch (OperationCanceledException) when (closeAfterExport) { }
        catch (Exception failure) { error = failure.Message + EarlierExportMessage(); }
        finally
        {
            exporting = false;
            UpdateControls();
            if (closeAfterExport) { closeAfterExport = false; Close(); }
        }
    }

    private string EarlierExportMessage() => exportedFolder is null ? "" :
        $" The earlier export of revision {exportedRevision} is still available.";

    private void OpenExportItem(string? path, Func<string, bool> exists)
    {
        if (path is null || closing || exporting) return;
        try
        {
            if (!exists(path)) throw new InvalidOperationException("The item has moved or been removed.");
            using var process = Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch (Exception failure) { error = failure.Message; UpdateControls(); }
    }

    private void Pump()
    {
        if (!closing) game.ValidateCapture();
        host.Pump();
        if (closing)
        {
            if (agentShutdown?.IsCompleted == true && host.WorkersRetired)
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
        var ready = !closing && !starting && !exporting && !chat.Busy;
        play.IsEnabled = ready && model.RuntimeId is null;
        stop.IsEnabled = ready && model.RuntimeId is not null;
        export.IsEnabled = play.IsEnabled;
        chat.IsEnabled = !closing && !exporting;
        showExport.IsVisible = exportedFolder is not null;
        showExport.IsEnabled = !closing && !exporting;
        showChecks.IsVisible = checkReport is not null;
        showChecks.IsEnabled = !closing && !exporting;
        var gameView = host.State["views"]?["game"];
        status.Text = closing ? "Closing project…" : closeAfterExport ? "Finishing export before closing…" :
            checking is not null ? "Checking the exported game…" : exporting ? "Exporting the saved project…" :
            starting ? "Starting preview…" : error ?? host.ViewError("game") ??
            model.PlaybackError ?? (previewSession is not null
                ? gameView?["presented_tick"] is not null ? "Click the game to take control. Press Esc to release the mouse." : "Waiting for the first game frame…"
                : model.RuntimeId is not null ? "A playtest is running. Stop it to start a preview."
                : exportedFolder is not null ? checkSummary ?? $"Exported revision {exportedRevision}. Open the build folder and run launch.cmd."
                : "Play opens the current project. Chat changes appear the next time you play.");
    }
}
