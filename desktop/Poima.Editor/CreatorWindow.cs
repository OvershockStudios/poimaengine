// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
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
    private readonly LaunchOptions options;
    private readonly NativeHost host;
    private readonly EditorModel model;
    private readonly GameInput game;
    private readonly AgentChat chat;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(33) };
    private readonly Button play = new() { Content = "Play" }, stop = new() { Content = "Stop" };
    private readonly Button export = new() { Content = "Export build" };
    private readonly Button playVerified = new() { Content = "Play verified build", IsVisible = false };
    private readonly Button showExport = new() { Content = "Open build folder", IsVisible = false };
    private readonly Button showChecks = new() { Content = "View playtest results", IsVisible = false };
    private readonly Button restore = new() { Content = "Restore verified project as copy", IsVisible = false };
    private readonly Button openRecovered = new() { Content = "Open recovered project", IsVisible = false };
    private readonly Button setupBuilds = new() { Content = "Set up gameplay builds", IsVisible = false };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Border viewport = new() { Background = EditorTheme.Brush("#161518"), ClipToBounds = true };
    private bool starting, exporting, closeAfterExport, verifiedPlayerRunning, closing, allowClose;
    private bool agentWasBusy, toolsReadyForReopen, reopenWithTools;
    private string? exportedFolder, verifiedCheckpoint, verifiedBundleHash, recoveredManifest;
    private PlaytestAttempt? latestAttempt;
    private string workDescription = "";
    private long exportedRevision;
    private Task? agentShutdown;
    private CancellationTokenSource? checking;
    private string? previewSession, error;

    public CreatorWindow(ProjectLocation project, LaunchOptions options)
    {
        this.project = project;
        this.options = options;
        Title = project.Name + " — Poima Creator";
        Width = 1180; Height = 760; MinWidth = 760; MinHeight = 560;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        UseLayoutRounding = true;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        host = new NativeHost(project.World, options.Endpoint, options.Gpu, options.Samples, options.ActiveDevelopmentProfiles);
        try
        {
            model = new EditorModel(host);
            game = new GameInput(model);
            game.Error += message => { error = message; UpdateControls(); };
            chat = new AgentChat(project.Root, host.Endpoint, creator: true,
                gameplayToolStatus: options.GameplayToolStatus, gameplayToolContext: options.GameplayToolContext,
                conversationStore: new CreatorConversationStore(project.Manifest, project.ProjectId));
            Content = BuildContent();
        }
        catch { host.Dispose(); throw; }
        timer.Tick += (_, _) => Pump();
        Opened += async (_, _) =>
        {
            UpdateControls();
            timer.Start();
            if (Path.GetDirectoryName(project.Root) is not null && Directory.Exists(HistoryDirectory()))
                await ProjectWork("Finding the last verified build…", LoadLatestVerification);
        };
        play.Click += async (_, _) => await PlayAsync();
        stop.Click += (_, _) => ExecuteStop();
        export.Click += async (_, _) => await ExportAsync();
        playVerified.Click += async (_, _) => await PlayVerifiedAsync();
        showExport.Click += (_, _) => OpenExportItem(exportedFolder, Directory.Exists);
        showChecks.Click += async (_, _) => await ShowPlaytestResultsAsync();
        restore.Click += async (_, _) => await RestoreVerifiedAsync();
        openRecovered.Click += (_, _) => OpenRecoveredProject();
        setupBuilds.IsVisible = OperatingSystem.IsWindows() && WindowsGameplayTools.HasAuthoringPack &&
            options.ActiveDevelopmentProfiles is null;
        setupBuilds.Click += async (_, _) =>
        {
            if (toolsReadyForReopen)
            {
                reopenWithTools = true;
                Close();
                if (!closing) reopenWithTools = false;
            }
            else if (await new GameplaySetupWindow().ShowDialog<bool>(this))
            {
                toolsReadyForReopen = true;
                setupBuilds.Content = "Reopen with build tools";
                error = "Build tools are ready. Reopening will close this conversation.";
                UpdateControls();
            }
        };
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
            if (reopenWithTools)
                try { using var process = Process.Start(CreatorStart(project.Manifest, reuseEndpoint: true)); }
                catch (Exception failure) when (failure is InvalidOperationException or System.ComponentModel.Win32Exception)
                { Console.Error.WriteLine(failure.Message); }
        };
    }

    private Control BuildContent()
    {
        var title = new StackPanel { Spacing = 4 };
        title.Children.Add(new TextBlock { Text = project.Name, FontSize = 24, FontWeight = FontWeight.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        title.Children.Add(new TextBlock { Text = project.Root, FontSize = 12, Foreground = EditorTheme.Brush("#B4ACB2"),
            TextTrimming = TextTrimming.CharacterEllipsis });
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), Margin = new Thickness(24), RowSpacing = 20 };
        setupBuilds.HorizontalAlignment = HorizontalAlignment.Right;
        setupBuilds.VerticalAlignment = VerticalAlignment.Top;
        var heading = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 16 };
        heading.Children.Add(title);
        Grid.SetColumn(setupBuilds, 1);
        heading.Children.Add(setupBuilds);
        root.Children.Add(heading);
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
        result.Children.Add(playVerified);
        result.Children.Add(showExport);
        result.Children.Add(showChecks);
        result.Children.Add(restore);
        result.Children.Add(openRecovered);
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

    private string ProjectParent() => Path.GetDirectoryName(project.Root) ??
        throw new InvalidOperationException("Move the project into its own folder before creating a build.");

    private string HistoryDirectory() => Path.Combine(ProjectParent(), ".poima-history", project.ProjectId);

    private async Task ExportAsync()
    {
        if (!export.IsEnabled || !export.IsVisible) return;
        await ProjectWork("Exporting the saved project…", async () =>
        {
            var current = await OpenCurrentProject();
            host.RefreshState();
            if (model.RuntimeId is not null || chat.Busy)
                throw new InvalidOperationException("Stop the playtest and wait for the agent before exporting.");
            if (host.Call("desktop.gameplay.inspect")["profile"] is not null)
                throw new InvalidOperationException("Clear the desktop gameplay profile and attach the published Native AOT game before exporting.");

            var name = Path.GetFileName(current.Root);
            if (name.Length > 160) name = "Poima";
            var suffix = Guid.NewGuid().ToString("N")[..8];
            var suite = Path.Combine(current.Root, ProjectFiles.VerificationSuiteName);
            var manifest = current.Manifest;
            string? checkpoint = null;
            string folder;
            if (File.Exists(suite))
            {
                var history = Directory.CreateDirectory(HistoryDirectory()).FullName;
                var time = DateTime.UtcNow.ToString("yyyyMMdd-HHmmssfff", CultureInfo.InvariantCulture);
                checkpoint = Path.Combine(history, $"{time}-{suffix}");
                var captured = await ProjectFiles.Command("checkpoint", manifest, "--output", checkpoint);
                manifest = captured.GetProperty("source_manifest").GetString()!;
                suite = Path.Combine(Path.GetDirectoryName(manifest)!, ProjectFiles.VerificationSuiteName);
                folder = Path.Combine(checkpoint, "game");
            }
            else folder = Path.Combine(ProjectParent(), $"{name} - Build {DateTime.Now:yyyy-MM-dd HHmmss} {suffix}");
            if (closeAfterExport) return;
            var result = await ProjectFiles.Command("build", manifest, "--output", folder, "--runtime", ProjectFiles.PreviewRuntime);
            var candidate = Path.GetFullPath(result.GetProperty("output").GetString()!);
            var revision = result.GetProperty("revision").GetInt64();
            if (closeAfterExport) return;
            if (checkpoint is null)
            {
                if (verifiedCheckpoint is not null)
                    error = $"Revision {revision} was exported without a check suite." + EarlierExportMessage();
                else
                {
                    exportedFolder = candidate;
                    exportedRevision = revision;
                }
                return;
            }
            using var cancellation = new CancellationTokenSource();
            checking = cancellation;
            workDescription = "Checking the exported game…";
            UpdateControls();
            var report = Path.Combine(checkpoint, "checks.json");
            var verification = await ProjectFiles.VerifyExport(candidate, suite, report, cancellation.Token);
            await LoadLatestVerification(cancellation.Token);
            if (latestAttempt is null || latestAttempt.Passed != verification.GetProperty("passed").GetBoolean() ||
                !string.Equals(Path.GetFullPath(latestAttempt.Report), Path.GetFullPath(report), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The exported game's results could not be paired with its source checkpoint.");
        });
    }

    private async Task LoadLatestVerification()
    {
        using var cancellation = new CancellationTokenSource();
        checking = cancellation;
        await LoadLatestVerification(cancellation.Token);
    }

    private async Task RefreshAgentHistory()
    {
        var previous = verifiedCheckpoint;
        var previousAttempt = latestAttempt;
        var priorError = error;
        await ProjectWork("Checking for a new verified build…", async () =>
        {
            await LoadLatestVerification();
            if (verifiedCheckpoint == previous && latestAttempt == previousAttempt)
                error = priorError;
        });
    }

    private async Task LoadLatestVerification(CancellationToken cancellation)
    {
        var history = await ReadHistory(cancellation);
        var latest = history.GetProperty("latest");
        var tested = history.GetProperty("latest_check");
        latestAttempt = tested.ValueKind == JsonValueKind.Object ? PlaytestAttempt.FromHistory(tested) : null;
        if (latest.ValueKind != JsonValueKind.Object)
        {
            if (verifiedCheckpoint is not null) exportedFolder = null;
            verifiedCheckpoint = null;
            verifiedBundleHash = null;
            return;
        }
        verifiedCheckpoint = latest.GetProperty("checkpoint").GetString();
        verifiedBundleHash = latest.GetProperty("bundle_sha256").GetString();
        exportedFolder = Path.GetDirectoryName(latest.GetProperty("game_manifest").GetString());
        exportedRevision = latest.GetProperty("revision").GetInt64();
    }

    private Task<JsonElement> ReadHistory(CancellationToken cancellation) =>
        ProjectFiles.Command(cancellation, "history", HistoryDirectory(), "--project-id", project.ProjectId);

    private async Task ShowPlaytestResultsAsync()
    {
        if (latestAttempt is not { } attempt || closing || exporting) return;
        try
        {
            using var report = await attempt.ReadReportAsync();
            if (closing || exporting || latestAttempt != attempt) return;
            await new PlaytestResultsWindow(attempt, report.RootElement, verifiedBundleHash, exportedRevision).ShowDialog(this);
        }
        catch (Exception failure) { error = failure.Message; UpdateControls(); }
    }

    private async Task PlayVerifiedAsync()
    {
        if (!playVerified.IsEnabled || exportedFolder is null || verifiedBundleHash is null) return;
        var manifest = Path.GetFullPath(Path.Combine(exportedFolder, "game.json"));
        var bundleHash = verifiedBundleHash;
        var priorError = error;
        await ProjectWork("Checking the verified build…", async () =>
        {
            using (var cancellation = new CancellationTokenSource())
            {
                checking = cancellation;
                var latest = (await ReadHistory(cancellation.Token)).GetProperty("latest");
                if (latest.ValueKind != JsonValueKind.Object ||
                    latest.GetProperty("bundle_sha256").GetString() != bundleHash ||
                    !string.Equals(Path.GetFullPath(latest.GetProperty("game_manifest").GetString()!), manifest, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("The selected verified build changed or is unavailable. Reopen the project to refresh it.");
                checking = null;
            }
            if (closeAfterExport) return;
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (!Path.IsPathFullyQualified(local))
                throw new InvalidOperationException("Poima could not locate local storage for playtest saves.");
            var saves = Directory.CreateDirectory(Path.Combine(local, "Poima", "Playtests", project.ProjectId, bundleHash)).FullName;
            verifiedPlayerRunning = true;
            workDescription = $"Playing verified revision {exportedRevision} in a separate window.";
            UpdateControls();
            try
            {
                await ProjectFiles.PlayExport(manifest, saves, options.Gpu, options.Samples);
                error = priorError;
            }
            finally { verifiedPlayerRunning = false; }
        }, describeEarlierExport: false);
    }

    private async Task RestoreVerifiedAsync()
    {
        if (!restore.IsEnabled || verifiedCheckpoint is null) return;
        var checkpoint = verifiedCheckpoint;
        await ProjectWork("Restoring a copy of the verified project…", async () =>
        {
            var name = Path.GetFileName(project.Root);
            if (name.Length > 140) name = "Poima";
            var suffix = Guid.NewGuid().ToString("N")[..8];
            var destination = Path.Combine(ProjectParent(), $"{name} - Recovered {suffix}");
            var result = await ProjectFiles.Command("recover", checkpoint, "--output", destination);
            recoveredManifest = result.GetProperty("restored_manifest").GetString();
        });
    }

    private void OpenRecoveredProject()
    {
        if (recoveredManifest is null || !openRecovered.IsEnabled) return;
        try
        {
            using var process = Process.Start(CreatorStart(recoveredManifest)) ??
                throw new InvalidOperationException("Poima could not open the recovered project.");
            Close();
        }
        catch (Exception failure) { error = failure.Message; UpdateControls(); }
    }

    private ProcessStartInfo CreatorStart(string manifest, bool reuseEndpoint = false)
    {
        var info = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "Poima.Editor.exe")) { UseShellExecute = false };
        info.ArgumentList.Add("--creator");
        info.ArgumentList.Add(manifest);
        if (reuseEndpoint)
        {
            info.ArgumentList.Add("--endpoint");
            info.ArgumentList.Add(options.Endpoint);
        }
        if (options.DevelopmentProfiles is not null)
        {
            info.ArgumentList.Add("--development-profiles");
            info.ArgumentList.Add(options.DevelopmentProfiles);
        }
        if (options.SoftwareUi) info.ArgumentList.Add("--software-ui");
        if (options.Gpu >= 0)
        {
            info.ArgumentList.Add("--gpu");
            info.ArgumentList.Add(options.Gpu.ToString(CultureInfo.InvariantCulture));
        }
        info.ArgumentList.Add("--samples");
        info.ArgumentList.Add(options.Samples.ToString(CultureInfo.InvariantCulture));
        return info;
    }

    private async Task ProjectWork(string description, Func<Task> work, bool describeEarlierExport = true)
    {
        exporting = true;
        error = null;
        workDescription = description;
        UpdateControls();
        try { await work(); }
        catch (OperationCanceledException) when (closeAfterExport) { }
        catch (Exception failure) { error = failure.Message + (describeEarlierExport ? EarlierExportMessage() : ""); }
        finally
        {
            checking = null;
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
        if (chat.Busy) agentWasBusy = true;
        else if (agentWasBusy && !starting && !exporting && model.RuntimeId is null)
        {
            agentWasBusy = false;
            if (Path.GetDirectoryName(project.Root) is not null && Directory.Exists(HistoryDirectory()))
                _ = RefreshAgentHistory();
        }
        UpdateControls();
    }

    private void UpdateControls()
    {
        var ready = !closing && !starting && !exporting && !chat.Busy;
        play.IsEnabled = ready && model.RuntimeId is null;
        stop.IsEnabled = ready && model.RuntimeId is not null;
        export.IsEnabled = play.IsEnabled;
        setupBuilds.IsEnabled = play.IsEnabled;
        chat.IsEnabled = !closing && !exporting;
        playVerified.IsVisible = verifiedBundleHash is not null;
        playVerified.IsEnabled = play.IsEnabled;
        showExport.IsVisible = exportedFolder is not null;
        showExport.IsEnabled = !closing && !exporting;
        showChecks.IsVisible = latestAttempt is not null;
        showChecks.IsEnabled = !closing && !exporting;
        restore.IsVisible = verifiedCheckpoint is not null;
        restore.IsEnabled = play.IsEnabled;
        openRecovered.IsVisible = recoveredManifest is not null;
        openRecovered.IsEnabled = play.IsEnabled;
        var gameView = host.State["views"]?["game"];
        status.Text = closing ? "Closing project…" : closeAfterExport
            ? verifiedPlayerRunning ? "Close the verified game window to finish closing Poima." : "Finishing project work before closing…" :
            exporting ? workDescription :
            starting ? "Starting preview…" : error ?? host.ViewError("game") ??
            model.PlaybackError ?? (previewSession is not null
                ? gameView?["presented_tick"] is not null ? "Click the game to take control. Press Esc to release the mouse." : "Waiting for the first game frame…"
                : model.RuntimeId is not null ? "A playtest is running. Stop it to start a preview."
                : recoveredManifest is not null ? "The verified project was recovered as an editable copy."
                : latestAttempt is { } attempt ? attempt.Summary + (!attempt.Retained && verifiedCheckpoint is not null
                    ? $" The last verified build, revision {exportedRevision}, is still playable." : "")
                : exportedFolder is not null ? $"Exported revision {exportedRevision}. Open the build folder and run launch.cmd."
                : "Play opens the current project. Chat changes appear the next time you play.");
    }
}
