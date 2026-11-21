// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Controls.Templates;
using Avalonia.Layout;
using Avalonia.Markup.Xaml.Styling;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Poima.AgentHost;

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
    private readonly Button shareExport = new() { Content = "Save game ZIP", IsVisible = false };
    private readonly Button showSharedZip = new() { Content = "Open ZIP folder", IsVisible = false };
    private readonly Button showChecks = new() { Content = "View playtest results", IsVisible = false };
    private readonly Button buildHistory = new() { Content = "Build history", IsVisible = false };
    private readonly Button restore = new() { Content = "Restore verified project as copy", IsVisible = false };
    private readonly Button openRecovered = new() { Content = "Open recovered project", IsVisible = false };
    private readonly Button setupBuilds = new() { Content = "Set up gameplay builds", IsVisible = false };
    private readonly Button projectSettings = new() { Content = EditorIcons.Make("settings") };
    private readonly Button importAssets = new() { Content = "Import assets…" };
    private readonly Button reviewSpecialists = new() { Content = "Specialist reviews" };
    private readonly Button addBuildWorker = new() { Content = "Add build worker" };
    private readonly Button reviewDrafts = new() { Content = "Review worker scenes", IsVisible = false };
    private readonly Button newWorkerRound = new() { Content = "Start new worker round", IsVisible = false };
    private readonly Button showCombined = new() { Content = "Combined game checks", IsVisible = false };
    private readonly Button reviewProposal = new() { Content = "Review proposed build", IsVisible = false };
    private readonly Button playCombined = new() { Content = "Play combined game", IsVisible = false };
    private readonly Button openCombined = new() { Content = "Open combined project as copy", IsVisible = false };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock verifiedPlayStatus = new() { TextWrapping = TextWrapping.Wrap, IsVisible = false };
    private readonly Border viewport = new() { Background = EditorTheme.Brush("#190F14"), ClipToBounds = true };
    private readonly StackPanel emptyPreview = new()
    {
        Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
        IsHitTestVisible = false
    };
    private bool starting, waitingForPreviewFrame, exporting, closeRequested, verifiedPlayActive, closing, allowClose;
    private bool agentWasBusy, toolsReadyForReopen, ownerDisposed;
    private (ProjectLocation Project, LaunchOptions Options)? reopen;
    private string? exportedFolder, verifiedCheckpoint, verifiedBundleHash, recoveredManifest;
    private VerifiedPlayback? verifiedPlayback;
    private string? sharedZip, shareNotice;
    private PlaytestAttempt? latestAttempt;
    private CreatorBuildTeam.CombinedCheck? combinedCheck, verifiedCombined;
    private string workDescription = "";
    private long exportedRevision;
    private Task? agentShutdown;
    private readonly TabControl conversations = new()
    {
        ItemsPanel = new FuncTemplate<Panel?>(() => new WrapPanel { ItemSpacing = 4, LineSpacing = 2 })
    };
    private readonly Grid conversationArea = new() { RowDefinitions = new RowDefinitions("*,Auto") };
    private readonly StackPanel conversationActions = new();
    private CreatorReviews? specialistReviews;
    private CreatorBuildTeam? buildTeam;
    private CancellationTokenSource? checking, checkingVerifiedPlay;
    private string? previewSession, error;

    public CreatorWindow(ProjectLocation project, LaunchOptions options, AgentChat.Draft? draft = null)
    {
        this.project = project;
        this.options = options;
        Title = project.Name + " — Poima Creator";
        Width = 1180; Height = 760; MinWidth = 760; MinHeight = 560;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        UseLayoutRounding = true;
        Background = EditorTheme.Brush("#211016");
        Foreground = EditorTheme.Brush("#F7E8EB");
        Styles.Add(new StyleInclude(new Uri("avares://Poima.Editor/"))
        { Source = new Uri("avares://Poima.Editor/Themes/CreatorControls.axaml") });
        host = new NativeHost(project.World, options.Endpoint, options.Gpu, options.Samples, options.ActiveDevelopmentProfiles);
        try
        {
            model = new EditorModel(host);
            game = new GameInput(model);
            game.Error += message => { error = message; UpdateControls(); };
            chat = new AgentChat(project.Root, host.Endpoint, creator: true,
                gameplayToolStatus: options.GameplayToolStatus, gameplayToolContext: options.GameplayToolContext,
                conversationStore: new CreatorConversationStore(project.Manifest, project.ProjectId),
                coordinateWorkers: CoordinateWorkersAsync);
            if (draft is not null) chat.RestoreDraft(draft);
            Content = BuildContent();
        }
        catch { host.Dispose(); throw; }
        timer.Tick += (_, _) => Pump();
        Opened += async (_, _) =>
        {
            UpdateControls();
            timer.Start();
            await ProjectWork("Restoring the workspace…", async () =>
            {
                using var cancellation = new CancellationTokenSource();
                checking = cancellation;
                try
                {
                    if (Path.GetDirectoryName(project.Root) is not null && Directory.Exists(HistoryDirectory()))
                        await LoadLatestVerification(cancellation.Token);
                }
                finally
                {
                    if (!closeRequested)
                        (combinedCheck, verifiedCombined) = await BuildTeam().RestoreAsync(cancellation.Token);
                }
            });
        };
        play.Click += async (_, _) => await PlayAsync();
        stop.Click += (_, _) => ExecuteStop();
        export.Click += async (_, _) => await ExportAsync();
        playVerified.Click += async (_, _) => await PlayVerifiedAsync();
        showExport.Click += (_, _) => OpenExportItem(exportedFolder, Directory.Exists);
        shareExport.Click += async (_, _) => await ShareVerifiedAsync();
        showSharedZip.Click += (_, _) =>
        {
            if (sharedZip is not null && File.Exists(sharedZip))
                OpenExportItem(Path.GetDirectoryName(sharedZip), Directory.Exists);
            else
            {
                sharedZip = null;
                shareNotice = null;
                error = "The game ZIP has moved or been removed.";
                UpdateControls();
            }
        };
        showChecks.Click += async (_, _) => await ShowPlaytestResultsAsync();
        buildHistory.Click += async (_, _) => await ShowBuildHistoryAsync();
        restore.Click += async (_, _) => await RestoreVerifiedAsync();
        openRecovered.Click += (_, _) => OpenRecoveredProject();
        importAssets.Click += async (_, _) => await ImportAssetsAsync();
        reviewSpecialists.Click += (_, _) => OpenSpecialistReviews();
        addBuildWorker.Click += async (_, _) => await AddBuildWorkerAsync();
        reviewDrafts.Click += async (_, _) => await ReviewDraftsAsync();
        newWorkerRound.Click += async (_, _) =>
        {
            if (!newWorkerRound.IsEnabled || buildTeam is not { } team) return;
            await ProjectWork("Starting a new worker round…", async () =>
            {
                await team.StartRoundAsync();
                combinedCheck = verifiedCombined = null;
            });
        };
        showCombined.Click += async (_, _) => await ShowCombinedChecksAsync();
        reviewProposal.Click += async (_, _) => await ReviewProposalAsync();
        playCombined.Click += async (_, _) => await PlayCombinedAsync();
        openCombined.Click += async (_, _) => await OpenCombinedAsync();
        projectSettings.Click += async (_, _) => await OpenProjectSettingsAsync();
        AutomationProperties.SetName(projectSettings, "Project settings");
        ToolTip.SetTip(projectSettings, "Project settings");
        ToolTip.SetTip(reviewSpecialists, "Connect the main Codex chat to open independent scene reviews.");
        ToolTip.SetTip(addBuildWorker, "Give a Codex worker its own editable draft.");
        setupBuilds.IsVisible = OperatingSystem.IsWindows() && WindowsGameplayTools.HasAuthoringPack &&
            options.ActiveDevelopmentProfiles is null;
        setupBuilds.Click += async (_, _) =>
        {
            if (toolsReadyForReopen)
            {
                (ProjectLocation Project, LaunchOptions Options)? configured = null;
                await ProjectWork("Preparing the gameplay build tools…", async () =>
                {
                    var current = await OpenCurrentProject();
                    var launch = await WindowsGameplayTools.ConfigureAsync(options, current);
                    if (launch.ActiveDevelopmentProfiles is null)
                        throw new InvalidOperationException(launch.GameplayToolStatus ?? "Poima could not set up gameplay builds.");
                    configured = (current, launch);
                });
                if (configured is null || closing || !IsVisible) return;
                reopen = configured;
                Close();
                if (!closing) reopen = null;
            }
            else if (await new GameplaySetupWindow().ShowDialog<bool>(this))
            {
                toolsReadyForReopen = true;
                setupBuilds.Content = "Reopen with build tools";
                error = "Build tools are ready. Reopen to activate them and keep your draft.";
                UpdateControls();
            }
        };
        Closing += (_, args) =>
        {
            if (allowClose) return;
            args.Cancel = true;
            if (closing) return;
            if (exporting || verifiedPlayActive)
            {
                closeRequested = true;
                checking?.Cancel();
                checkingVerifiedPlay?.Cancel();
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
                agentShutdown = Task.WhenAll(chat.CloseAsync(), specialistReviews?.RetireAsync() ?? Task.CompletedTask,
                    buildTeam?.RetireAsync() ?? Task.CompletedTask);
            }
            catch (Exception failure) { error = failure.Message; }
            UpdateControls();
        };
        Closed += (_, _) => DisposeOwner();
    }

    private void DisposeOwner()
    {
        if (ownerDisposed) return;
        ownerDisposed = true;
        timer.Stop();
        model.Dispose();
        host.Dispose();
    }

    private void OpenWithBuildTools(ProjectLocation current, LaunchOptions configured, AgentChat.Draft draft)
    {
        CreatorWindow? replacement = null;
        try
        {
            if (Application.Current?.ApplicationLifetime is not IClassicDesktopStyleApplicationLifetime desktop ||
                desktop.MainWindow != this)
                throw new InvalidOperationException("The creator workspace is no longer active.");
            replacement = new CreatorWindow(current, configured, draft);
            replacement.Show();
            desktop.MainWindow = replacement;
        }
        catch (Exception failure)
        {
            replacement?.DisposeOwner();
            var retry = new Button { Content = "Retry opening the project" };
            retry.Click += (_, _) => OpenWithBuildTools(current, configured, draft);
            Content = new StackPanel { Margin = new Thickness(26), Spacing = 12, Children =
                {
                    new TextBlock { Text = "Couldn’t reopen the project. " + failure.Message, TextWrapping = TextWrapping.Wrap },
                    new TextBox { Text = draft.Text, IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap },
                    retry
                } };
            return;
        }
        Close();
    }

    private Control BuildContent()
    {
        var title = new TextBlock { Text = project.Name, FontSize = 26, FontWeight = FontWeight.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
        ToolTip.SetTip(title, project.Root);
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), Margin = new Thickness(24), RowSpacing = 18 };
        var heading = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 16 };
        heading.Children.Add(title);
        var projectActions = new WrapPanel { ItemSpacing = 8, VerticalAlignment = VerticalAlignment.Top,
            Children = { importAssets, reviewSpecialists, addBuildWorker, setupBuilds, projectSettings } };
        Grid.SetColumn(projectActions, 1);
        heading.Children.Add(projectActions);
        root.Children.Add(heading);
        var panels = new Grid { ColumnDefinitions = new ColumnDefinitions("5*,7*"), ColumnSpacing = 16 };
        conversations.Items.Add(ConversationTab("Main chat", chat));
        conversations.SelectedIndex = 0;
        conversations.SelectionChanged += (_, _) =>
        {
            if (specialistReviews is { } team)
                team.Footer.IsVisible = conversations.SelectedItem is TabItem selected && team.Tabs.Contains(selected);
        };
        conversationArea.Children.Add(conversations);
        conversationActions.Children.Add(reviewDrafts);
        conversationActions.Children.Add(newWorkerRound);
        Grid.SetRow(conversationActions, 1);
        conversationArea.Children.Add(conversationActions);
        panels.Children.Add(conversationArea);
        var preview = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 14, Margin = new Thickness(16) };
        var bar = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        bar.Children.Add(new TextBlock { Text = "Game preview", FontSize = 20,
            FontWeight = FontWeight.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        var transport = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        play.Classes.Add("primary");
        AutomationProperties.SetName(play, "Play project");
        AutomationProperties.SetName(stop, "Stop preview");
        AutomationProperties.SetName(export, "Export Windows build");
        export.IsVisible = File.Exists(Path.Combine(ProjectFiles.PreviewRuntime, "runtime.json"));
        transport.Children.Add(play); transport.Children.Add(stop);
        transport.Children.Add(export);
        Grid.SetColumn(transport, 1); bar.Children.Add(transport);
        preview.Children.Add(bar);
        var previewSurface = new Grid { Children = { viewport, emptyPreview } };
        emptyPreview.Children.Add(EditorIcons.Make("play", 28));
        emptyPreview.Children.Add(new TextBlock { Text = "Preview will appear here", Foreground = EditorTheme.Brush("#EBC7CE"),
            HorizontalAlignment = HorizontalAlignment.Center });
        Grid.SetRow(previewSurface, 1); preview.Children.Add(previewSurface);
        AutomationProperties.SetAutomationId(status, "preview-status");
        AutomationProperties.SetAutomationId(verifiedPlayStatus, "verified-play-status");
        var result = new StackPanel { Spacing = 8 };
        result.Children.Add(status);
        result.Children.Add(playVerified);
        result.Children.Add(verifiedPlayStatus);
        var sharing = new WrapPanel { ItemSpacing = 8, LineSpacing = 8 };
        sharing.Children.Add(showExport);
        sharing.Children.Add(shareExport);
        sharing.Children.Add(showSharedZip);
        result.Children.Add(sharing);
        result.Children.Add(new WrapPanel { ItemSpacing = 8, LineSpacing = 8, Children = { showChecks, buildHistory } });
        result.Children.Add(new WrapPanel { ItemSpacing = 8, LineSpacing = 8,
            Children = { reviewProposal, showCombined, playCombined, openCombined } });
        result.Children.Add(restore);
        result.Children.Add(openRecovered);
        Grid.SetRow(result, 2); preview.Children.Add(result);
        var previewCard = Card(preview);
        Grid.SetColumn(previewCard, 1); panels.Children.Add(previewCard);
        Grid.SetRow(panels, 1); root.Children.Add(panels);
        return root;
    }

    private static Border Card(Control child) => new() { Child = child, Background = EditorTheme.Brush("#2B1A23"),
        BorderBrush = EditorTheme.Brush("#513641"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(9),
        ClipToBounds = true };

    internal static TabItem ConversationTab(string name, Control content)
    {
        var tab = new TabItem { Header = name, Content = content };
        AutomationProperties.SetName(tab, name);
        return tab;
    }

    private async Task ImportAssetsAsync()
    {
        if (!importAssets.IsEnabled) return;
        var selected = await new CreatorAssetWindow(host).ShowDialog<CreatorAsset[]?>(this);
        if (!closing && selected is { Length: > 0 }) chat.AddAssetReferences(selected);
    }

    private Task OpenProjectSettingsAsync() => ProjectWork("Opening project settings…", async () =>
    {
        var current = await ProjectFiles.Command("inspect", project.Manifest);
        if (closeRequested || closing) return;
        var hash = current.GetProperty("manifest_sha256").GetString()!;
        var splash = new CheckBox
        {
            Content = "Show the Poima splash in exported games",
            IsChecked = current.GetProperty("startup_splash").GetBoolean()
        };
        AutomationProperties.SetName(splash, "Show the Poima splash in exported games");
        var save = new Button { Content = "Save", IsDefault = true };
        save.Classes.Add("primary");
        var cancel = new Button { Content = "Cancel", IsCancel = true };
        var dialog = new Window
        {
            Title = "Project settings", Width = 460, SizeToContent = SizeToContent.Height, CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, ShowInTaskbar = false,
            FontFamily = FontFamily, Background = Background, Foreground = Foreground,
            Content = new StackPanel
            {
                Spacing = 16, Margin = new Thickness(24), Children =
                {
                    splash,
                    new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right,
                        Spacing = 8, Children = { cancel, save } }
                }
            }
        };
        dialog.Styles.Add(new StyleInclude(new Uri("avares://Poima.Editor/"))
        { Source = new Uri("avares://Poima.Editor/Themes/CreatorControls.axaml") });
        save.Click += (_, _) => dialog.Close(splash.IsChecked == true);
        cancel.Click += (_, _) => dialog.Close();
        if (await dialog.ShowDialog<bool?>(this) is bool enabled)
        {
            await ProjectFiles.Command("splash", project.Manifest, "--enabled", enabled ? "true" : "false",
                "--expected-manifest-sha256", hash);
        }
    });

    private async Task PlayAsync()
    {
        if (!play.IsEnabled) return;
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
            host.RefreshState();
            viewport.Child = new VulkanView(host, "game", game);
            previewSession = session;
            waitingForPreviewFrame = true;
        }
        catch (Exception failure)
        {
            if (started is not null) AbandonPreview(started, failure.Message);
            else { error = failure.Message; ClearPreview(); }
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
            ClearPreview();
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

    private void ClearPreview()
    {
        viewport.Child = null;
        previewSession = null;
        waitingForPreviewFrame = false;
    }

    private void StartAfterFirstFrame(string session)
    {
        var view = host.State["views"]?["game"];
        if (view?["graphics_error"]?.GetValue<string>() is { } fault)
        {
            AbandonPreview(session, "Couldn’t start the Vulkan game preview. Check your graphics driver and try again. " + fault);
            return;
        }
        if (view?["presented_tick"] is null) return;
        try
        {
            host.Call("desktop.play.resume", new() { ["session_id"] = session });
            waitingForPreviewFrame = false;
            host.RefreshState();
        }
        catch (Exception failure) { AbandonPreview(session, failure.Message); }
    }

    private void AbandonPreview(string session, string reason)
    {
        error = reason;
        try { StopRuntime(session); }
        catch (Exception cleanup) { error += " Unable to stop the preview. " + cleanup.Message; }
        finally { ClearPreview(); }
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
            if (closeRequested) return;
            var result = await ProjectFiles.Command("build", manifest, "--output", folder, "--runtime", ProjectFiles.PreviewRuntime);
            var candidate = Path.GetFullPath(result.GetProperty("output").GetString()!);
            var revision = result.GetProperty("revision").GetInt64();
            if (closeRequested) return;
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

    private async Task ShowBuildHistoryAsync()
    {
        if (!buildHistory.IsEnabled) return;
        try
        {
            var selected = await new BuildHistoryWindow(ReadHistoryPage, verifiedBundleHash, exportedRevision)
                .ShowDialog<BuildHistoryEntry?>(this);
            if (selected is not null && buildHistory.IsEnabled)
                await RestoreCheckpointAsync(selected.Checkpoint, selected.Snapshot);
        }
        catch (Exception failure) { error = failure.Message; UpdateControls(); }
    }

    private Task<JsonElement> ReadHistoryPage(string? before, CancellationToken cancellation)
    {
        string[] arguments = ["versions", HistoryDirectory(), "--project-id", project.ProjectId];
        return ProjectFiles.Command(cancellation, before is null ? arguments : [.. arguments, "--before", before]);
    }

    private async Task PlayVerifiedAsync()
    {
        if (!playVerified.IsEnabled || exportedFolder is null || verifiedBundleHash is null) return;
        await PlayVerifiedAsync(project.ProjectId, HistoryDirectory(), Path.GetFullPath(Path.Combine(exportedFolder, "game.json")),
            verifiedBundleHash, exportedRevision);
    }

    private Task PlayCombinedAsync() => playCombined.IsEnabled && verifiedCombined is { } combined
        ? PlayVerifiedAsync(combined.ProjectId, combined.History, combined.GameManifest, combined.Attempt.Build, combined.Attempt.Revision)
        : Task.CompletedTask;

    private async Task PlayVerifiedAsync(string projectId, string history, string manifest, string bundleHash, long revision)
    {
        manifest = Path.GetFullPath(manifest);
        var playback = verifiedPlayback = new VerifiedPlayback(bundleHash, revision);
        verifiedPlayActive = true;
        verifiedPlayStatus.Text = $"Checking verified revision {revision}…";
        UpdateControls();
        try
        {
            PublishVerifiedPlayback();
            using (var cancellation = new CancellationTokenSource())
            {
                checkingVerifiedPlay = cancellation;
                try
                {
                    var latest = (await ProjectFiles.Command(cancellation.Token, "history", history,
                        "--project-id", projectId)).GetProperty("latest");
                    if (latest.ValueKind != JsonValueKind.Object ||
                        latest.GetProperty("bundle_sha256").GetString() != bundleHash ||
                        !string.Equals(Path.GetFullPath(latest.GetProperty("game_manifest").GetString()!), manifest, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("The selected verified build changed or is unavailable. Reopen the project to refresh it.");
                }
                finally { checkingVerifiedPlay = null; }
            }
            if (closeRequested) { playback.Ended("cancelled"); return; }
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (!Path.IsPathFullyQualified(local))
                throw new InvalidOperationException("Poima could not locate local storage for playtest saves.");
            var saves = Directory.CreateDirectory(Path.Combine(local, "Poima", "Playtests", projectId, bundleHash)).FullName;
            verifiedPlayStatus.Text = $"Preparing verified revision {revision}…";
            UpdateControls();
            using var preparation = new CancellationTokenSource();
            checkingVerifiedPlay = preparation;
            var result = await ProjectFiles.PlayExport(manifest, saves, options.Gpu, options.Samples, playback,
                preparation.Token, () => checkingVerifiedPlay = null);
            var played = result.GetProperty("play");
            playback.Closed(played);
            Console.WriteLine(JsonSerializer.Serialize(new
            {
                event_type = "verified_play_closed", revision, bundle_sha256 = bundleHash,
                tick = played.GetProperty("tick"), frames_presented = played.GetProperty("frames_presented"),
                stop_reason = played.GetProperty("stop_reason")
            }));
        }
        catch (OperationCanceledException) when (closeRequested) { playback.Ended("cancelled"); }
        catch (Exception failure) { playback.Ended("failed", failure); error = failure.Message; }
        finally
        {
            checkingVerifiedPlay = null;
            PublishVerifiedPlayback();
            verifiedPlayStatus.Text = null;
            verifiedPlayActive = false;
            UpdateControls();
            CloseWhenWorkEnds();
        }
    }

    private void PublishVerifiedPlayback()
    {
        if (verifiedPlayback?.TakeSnapshot() is not { } snapshot) return;
        if (snapshot["state"]?.GetValue<string>() == "running")
            verifiedPlayStatus.Text = $"Verified revision {snapshot["revision"]!.GetValue<long>()} is running in a separate window.";
        try { host.PublishVerifiedPlayback(snapshot); }
        catch (InvalidOperationException failure) { error = failure.Message; }
    }

    private Task RestoreVerifiedAsync() => restore.IsEnabled && verifiedCheckpoint is { } checkpoint
        ? RestoreCheckpointAsync(checkpoint) : Task.CompletedTask;

    private async Task RestoreCheckpointAsync(string checkpoint, string? snapshot = null)
    {
        await ProjectWork("Restoring a copy of the verified project…", async () =>
        {
            var name = Path.GetFileName(project.Root);
            if (name.Length > 140) name = "Poima";
            var suffix = Guid.NewGuid().ToString("N")[..8];
            var destination = Path.Combine(ProjectParent(), $"{name} - Recovered {suffix}");
            string[] arguments = ["recover", checkpoint, "--output", destination];
            if (snapshot is not null) arguments = [.. arguments, "--expected-snapshot-sha256", snapshot];
            var result = await ProjectFiles.Command(arguments);
            recoveredManifest = result.GetProperty("restored_manifest").GetString();
        });
    }

    private async Task ShareVerifiedAsync()
    {
        if (!shareExport.IsEnabled || verifiedCheckpoint is not { } checkpoint || verifiedBundleHash is not { } bundle)
            return;
        await ProjectWork("Choose where to save the game…", async () =>
        {
            var leaf = Path.GetFileName(project.Root);
            var name = leaf.Length <= 100 ? leaf : "Poima Game";
            var destination = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                Title = "Save game ZIP",
                SuggestedStartLocation = await StorageProvider.TryGetFolderFromPathAsync(ProjectParent()),
                SuggestedFileName = $"{name} r{exportedRevision}",
                DefaultExtension = "zip",
                FileTypeChoices = [new FilePickerFileType("ZIP archive") { Patterns = ["*.zip"] }],
                ShowOverwritePrompt = false,
            });
            if (destination is null || closeRequested) return;
            var path = destination.TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local folder for the game ZIP.");
            if (IsInside(path, project.Root) || IsInside(path, HistoryDirectory()))
                throw new InvalidOperationException("Save the ZIP outside the editable project and its build history.");
            workDescription = "Saving the verified game ZIP…";
            UpdateControls();
            var result = await ProjectFiles.Command("share", checkpoint, "--expected-bundle-sha256", bundle, "--output", path);
            sharedZip = result.GetProperty("archive").GetString()!;
            shareNotice = $"Saved {Path.GetFileName(sharedZip)}. Extract the ZIP and open launch.cmd to play.";
        });
    }

    private static bool IsInside(string file, string directory)
    {
        var relative = Path.GetRelativePath(directory, file);
        return !Path.IsPathRooted(relative) && relative != ".." &&
            !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal);
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

    private ProcessStartInfo CreatorStart(string manifest)
    {
        var info = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "Poima.Editor.exe")) { UseShellExecute = false };
        info.ArgumentList.Add("--creator");
        info.ArgumentList.Add(manifest);
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

    private async Task ProjectWork(string description, Func<Task> work)
    {
        exporting = true;
        error = null;
        shareNotice = null;
        workDescription = description;
        UpdateControls();
        try { await work(); }
        catch (OperationCanceledException) when (closeRequested) { }
        catch (Exception failure) { error = failure.Message + EarlierExportMessage(); }
        finally
        {
            checking = null;
            exporting = false;
            UpdateControls();
            CloseWhenWorkEnds();
        }
    }

    private void CloseWhenWorkEnds()
    {
        if (!closeRequested || exporting || verifiedPlayActive) return;
        closeRequested = false;
        Close();
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
        if (verifiedPlayActive) PublishVerifiedPlayback();
        host.Pump();
        if (closing)
        {
            if (agentShutdown?.IsCompleted == true && host.WorkersRetired)
            {
                allowClose = true;
                if (reopen is { } configured)
                {
                    var draft = chat.CaptureDraft();
                    DisposeOwner();
                    OpenWithBuildTools(configured.Project, configured.Options, draft);
                }
                else Close();
            }
            return;
        }
        if (previewSession is not null && model.RuntimeId != previewSession) ClearPreview();
        if (waitingForPreviewFrame && previewSession is string session) StartAfterFirstFrame(session);
        if (previewSession is not null && !waitingForPreviewFrame) game.Tick();
        if (chat.Busy) agentWasBusy = true;
        else if (agentWasBusy && !closeRequested && !starting && !exporting && model.RuntimeId is null)
        {
            agentWasBusy = false;
            if (Path.GetDirectoryName(project.Root) is not null && Directory.Exists(HistoryDirectory()))
                _ = RefreshAgentHistory();
        }
        UpdateControls();
    }

    private void UpdateControls()
    {
        var ready = !closing && !closeRequested && !starting && !exporting && buildTeam?.IsVerifying != true;
        projectSettings.IsEnabled = ready;
        var canPreview = ready && !verifiedPlayActive && model.RuntimeId is null;
        play.IsEnabled = canPreview && !chat.Busy;
        stop.IsEnabled = ready && !chat.Busy && model.RuntimeId is not null;
        stop.IsVisible = model.RuntimeId is not null;
        emptyPreview.IsVisible = previewSession is null && !starting;
        export.IsEnabled = play.IsEnabled;
        importAssets.IsEnabled = play.IsEnabled;
        reviewSpecialists.IsEnabled = ready && (specialistReviews is not null || chat.ReviewClient is not null);
        var codexConnected = chat.ReviewClient is not null;
        buildTeam?.UpdateConnection(codexConnected);
        addBuildWorker.IsEnabled = ready && codexConnected && buildTeam?.CanAdd == true;
        reviewDrafts.IsVisible = buildTeam?.HasWorkers == true && export.IsVisible;
        reviewDrafts.IsEnabled = play.IsEnabled && buildTeam?.CanReview == true;
        newWorkerRound.IsVisible = buildTeam?.CanStartRound == true;
        newWorkerRound.IsEnabled = play.IsEnabled && newWorkerRound.IsVisible;
        reviewProposal.IsVisible = buildTeam?.Proposal is not null;
        reviewProposal.IsEnabled = canPreview;
        showCombined.IsVisible = combinedCheck is not null && combinedCheck != buildTeam?.Proposal;
        showCombined.IsEnabled = ready;
        playCombined.IsVisible = openCombined.IsVisible = verifiedCombined is not null;
        playCombined.IsEnabled = openCombined.IsEnabled = play.IsEnabled;
        setupBuilds.IsEnabled = play.IsEnabled;
        chat.IsEnabled = !closing && !closeRequested && !exporting;
        playVerified.IsVisible = verifiedBundleHash is not null;
        playVerified.IsEnabled = canPreview;
        verifiedPlayStatus.IsVisible = verifiedPlayActive;
        showExport.IsVisible = exportedFolder is not null;
        showExport.IsEnabled = !closing && !closeRequested && !exporting;
        shareExport.IsVisible = verifiedCheckpoint is not null;
        shareExport.IsEnabled = play.IsEnabled;
        showSharedZip.IsVisible = sharedZip is not null;
        showSharedZip.IsEnabled = showExport.IsEnabled;
        showChecks.IsVisible = latestAttempt is not null;
        showChecks.IsEnabled = !closing && !closeRequested && !exporting;
        buildHistory.IsVisible = latestAttempt is not null;
        buildHistory.IsEnabled = play.IsEnabled;
        restore.IsVisible = verifiedCheckpoint is not null;
        restore.IsEnabled = play.IsEnabled;
        openRecovered.IsVisible = recoveredManifest is not null;
        openRecovered.IsEnabled = play.IsEnabled;
        var gameView = host.State["views"]?["game"];
        status.Text = closing ? "Closing project…" : closeRequested
            ? verifiedPlayActive && checkingVerifiedPlay is null ? "Close the verified game window to finish closing Poima." : "Finishing project work before closing…" :
            exporting ? workDescription :
            starting ? "Starting preview…" : error ?? host.ViewError("game") ??
            model.PlaybackError ?? (previewSession is not null
                ? gameView?["presented_tick"] is not null ? "Click the game to take control. Press Esc to release the mouse." : "Waiting for the first game frame…"
                : model.RuntimeId is not null ? "A playtest is running. Stop it to start a preview."
                : shareNotice is not null ? shareNotice
                : recoveredManifest is not null ? "The verified project was recovered as an editable copy."
                : latestAttempt is { } attempt ? attempt.Summary + (!attempt.Retained && verifiedCheckpoint is not null
                    ? $" The last verified build, revision {exportedRevision}, is still playable." : "")
                : exportedFolder is not null ? $"Exported revision {exportedRevision}. Open the build folder and run launch.cmd."
                : "Play opens the current project. Chat changes appear the next time you play.");
    }

    private async Task AddBuildWorkerAsync()
    {
        if (!addBuildWorker.IsEnabled || chat.ReviewClient is null) return;
        await BuildTeam().AddAsync();
        UpdateControls();
    }

    private CreatorBuildTeam BuildTeam() => buildTeam ??= new CreatorBuildTeam(project, conversations, finding =>
        {
            if (!chat.AddReviewToDraft(finding)) return false;
            conversations.SelectedIndex = 0;
            return true;
        }, message => { error = message; UpdateControls(); },
        result => combinedCheck = result, () => chat.ReviewClient);

    private Task<JsonObject> CoordinateWorkersAsync(CodexWorkerRequest request, CancellationToken cancellation)
    {
        if (closing || closeRequested || chat.ReviewClient is null)
            throw new InvalidOperationException("Connect the main Codex chat in an open creator workspace.");
        if (request.ExpectedRevision is not null || request.Drafts is not null || request.FollowUp is not null)
        {
            host.RefreshState();
            if (request.ExpectedRevision is not null && (exporting || starting || model.RuntimeId is not null))
                throw new InvalidOperationException("Stop the project preview or wait for project work before assigning scene workers.");
        }
        return BuildTeam().CoordinateAsync(request, !exporting && !starting && !verifiedPlayActive && model.RuntimeId is null, cancellation);
    }

    private Task ReviewDraftsAsync()
    {
        if (!reviewDrafts.IsEnabled || buildTeam is not { } team) return Task.CompletedTask;
        return ProjectWork("Loading worker scenes…", async () =>
        {
            using var cancellation = new CancellationTokenSource();
            checking = cancellation;
            var drafts = await team.ReviewAsync(cancellation.Token);
            if (closeRequested) return;
            if (drafts.Length == 0) throw new InvalidOperationException("Wait for a worker to finish its draft.");
            var selected = await new CreatorDraftReviewWindow(team.BaseRevision, drafts)
                .ShowDialog<CreatorBuildTeam.ReviewedDraft[]?>(this);
            if (closeRequested || selected is not { Length: > 0 }) return;
            workDescription = "Combining the selected scenes…";
            UpdateControls();
            var result = await team.CombineAsync(selected, progress =>
            {
                workDescription = progress;
                UpdateControls();
            }, cancellation.Token);
            combinedCheck = result;
            if (result.Attempt.Retained) verifiedCombined = result;
            error = "Combined game. " + result.Attempt.Summary +
                (!result.Attempt.Passed && verifiedCombined is not null ? " The previous combined game is still playable." : "");
        });
    }

    private async Task ShowCombinedChecksAsync()
    {
        if (!showCombined.IsEnabled || combinedCheck is not { } combined) return;
        try
        {
            using var report = await combined.Attempt.ReadReportAsync();
            if (!showCombined.IsEnabled || combinedCheck != combined) return;
            await new PlaytestResultsWindow(combined.Attempt, report.RootElement,
                verifiedCombined?.Attempt.Build, verifiedCombined?.Attempt.Revision ?? 0).ShowDialog(this);
        }
        catch (Exception failure) { error = failure.Message; UpdateControls(); }
    }

    private Task ReviewProposalAsync()
    {
        if (!reviewProposal.IsEnabled || buildTeam is not { Proposal: { } proposal } team) return Task.CompletedTask;
        return ProjectWork("Opening the proposed build results…", async () =>
        {
            using var cancellation = new CancellationTokenSource();
            checking = cancellation;
            using var report = await proposal.Attempt.ReadReportAsync();
            if (closeRequested) return;
            var review = new PlaytestResultsWindow(proposal.Attempt, report.RootElement, verifiedCombined?.Attempt.Build,
                verifiedCombined?.Attempt.Revision ?? 0, approveBuild: true);
            if (!await review.ShowDialog<bool>(this) || closeRequested) return;
            verifiedCombined = await team.AcceptAsync(proposal, cancellation.Token);
            shareNotice = "The approved build is ready to play.";
        });
    }

    private Task OpenCombinedAsync()
    {
        if (!openCombined.IsEnabled || verifiedCombined is not { } combined) return Task.CompletedTask;
        return ProjectWork("Opening a copy of the verified combined project…", async () =>
        {
            var name = Path.GetFileName(project.Root);
            if (name.Length > 140) name = "Poima";
            var output = Path.Combine(ProjectParent(), $"{name} - Combined {Guid.NewGuid().ToString("N")[..8]}");
            var recovered = await ProjectFiles.Command("recover", combined.Checkpoint,
                "--output", output, "--expected-snapshot-sha256", combined.Snapshot);
            if (closeRequested) return;
            var manifest = Path.GetFullPath(recovered.GetProperty("restored_manifest").GetString()!);
            using var process = Process.Start(CreatorStart(manifest)) ??
                throw new InvalidOperationException("Poima could not open the combined project.");
            shareNotice = "Opened the verified combined project as an editable copy.";
        });
    }

    private void OpenSpecialistReviews()
    {
        if (specialistReviews is not null) { conversations.SelectedItem = specialistReviews.Tabs[0]; return; }
        if (chat.ReviewClient is not { } client) return;
        try
        {
            specialistReviews = new CreatorReviews(host, project.Root, client.Executable, client.Model, finding =>
            {
                if (!chat.AddReviewToDraft(finding)) return false;
                conversations.SelectedIndex = 0;
                return true;
            });
            foreach (var tab in specialistReviews.Tabs) conversations.Items.Add(tab);
            conversationActions.Children.Insert(0, specialistReviews.Footer);
            conversations.SelectedItem = specialistReviews.Tabs[0];
        }
        catch (Exception failure) { specialistReviews = null; error = failure.Message; UpdateControls(); }
    }
}
