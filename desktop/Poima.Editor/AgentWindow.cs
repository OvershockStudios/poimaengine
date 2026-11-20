// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Poima.AgentHost;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Poima.Editor;

/// <summary>Chat client over the shared engine endpoint; never calls NativeHost from a worker.</summary>
internal sealed class AgentChat : UserControl
{
    private readonly string projectRoot, endpoint;
    private readonly bool creator;
    private readonly string? gameplayToolContext;
    private readonly CreatorConversationStore? conversationStore;
    private readonly CancellationTokenSource lifetime = new();
    private readonly ComboBox provider = new() { ItemsSource = new[] { "Codex", "Antigravity", "Claude Code" }, SelectedIndex = 0 };
    private readonly TextBox executable = Field("Codex executable", "codex.exe");
    private readonly TextBox agyExecutable = Field("Antigravity CLI executable", FindAntigravity());
    private readonly TextBox claudeExecutable = Field("Claude ACP executable", FindClaudeAdapter());
    private readonly TextBox engine = Field("Poima executable", Path.Combine(AppContext.BaseDirectory, "poima.exe"));
    private readonly TextBox thread = Field("Conversation ID (empty starts a new conversation)", "");
    private readonly TextBox model = Field("Model (empty uses the CLI's advertised default)", "");
    private readonly CheckBox allowEdits = new() { Content = "Allow Poima world edits in this session", IsChecked = true };
    private readonly CheckBox allowSourceEdits = new() { Content = "Allow gameplay source edits in this session" };
    private readonly CheckBox allowBuilds = new() { Content = "Allow gameplay builds in this session" };
    private readonly CheckBox allowChecks = new() { Content = "Allow gameplay check edits in this session" };
    private readonly TextBox prompt = new() { AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 85, MaxHeight = 180 };
    private readonly TextBox transcript = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock status = new() { Text = "Disconnected", TextWrapping = TextWrapping.Wrap };
    private readonly StackPanel approvals = new() { Spacing = 6 };
    private readonly ScrollViewer approvalScroll;
    private readonly Button addImages = new() { Content = "Add images", IsEnabled = false, IsVisible = false };
    private readonly StackPanel imageChips = new() { Spacing = 3, IsVisible = false };
    private readonly List<(string Name, AgentImage Image, Control Chip)> images = [];
    private readonly Button connect = new() { Content = "Connect" }, disconnect = new() { Content = "Disconnect", IsEnabled = false },
        send = new() { Content = "Send", IsEnabled = false }, stop = new() { Content = "Stop", IsEnabled = false };
    private readonly Button getCodex = new() { Content = WindowsCodexInstall.Supported ? "Install Codex" : "Get Codex", IsVisible = false };
    private readonly Button newChat = new() { Content = "New chat", IsVisible = false };
    private IAgentSession? session;
    private CodexSignInWindow? signIn;
    private CodexInstallWindow? codexInstall;
    private CodexQuestionCard? questionCard;
    private string? activeTurn, unusedConversation;
    private string? rememberedThread, displayedThread, submittedCodexTurn, lastSuccessfulCodexTurn;
    private bool historyUnavailable;
    private readonly HashSet<string> completed = new(StringComparer.Ordinal);
    private readonly Dictionary<StackPanel, JsonNode> acpPermissions = [];
    private readonly Dictionary<StackPanel, JsonNode> codexPoimaApprovals = [];
    private readonly Dictionary<string, string> acpToolDetails = new(StringComparer.Ordinal);
    private bool submitting, closing, connecting, ready, cancelRequested, sentGameplayContext;
    private bool acpReceivedReply, codexSupportsImages, pickingImages, codexMissing;
    private Task? connectingTask, closingTask, retiringTask, installingTask;
    public bool Busy => submitting || activeTurn is not null || connecting || codexInstall is not null || retiringTask?.IsCompleted == false;
    private string ProviderName => provider.SelectedItem as string ?? "Codex";
    private string? ResumeThread => string.IsNullOrWhiteSpace(thread.Text) || thread.Text.Trim() == unusedConversation
        ? null : thread.Text.Trim();
    public AgentChat(string projectRoot, string endpoint, bool creator = false,
        string? gameplayToolStatus = null, string? gameplayToolContext = null, CreatorConversationStore? conversationStore = null)
    {
        this.projectRoot = projectRoot; this.endpoint = endpoint; this.creator = creator;
        this.gameplayToolContext = gameplayToolContext;
        this.conversationStore = conversationStore;
        if (conversationStore is not null)
        {
            try { thread.Text = rememberedThread = conversationStore.Read(); }
            catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException)
            { status.Text = "Poima couldn’t read the saved Codex chat. You can start a new chat."; }
        }
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = creator ? 14 : 12;
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto,Auto,Auto"), Margin = new Thickness(14), RowSpacing = 8 };
        prompt.PlaceholderText = creator ? "Describe the game you want to make…" : "Ask about this project…";
        var setup = new StackPanel { Spacing = 6 };
        AutomationProperties.SetName(provider, "Agent provider");
        if (creator)
        {
            executable.Text = FindCodex();
            allowEdits.IsChecked = false;
            allowEdits.Content = "Allow Poima world calls without asking";
            ToolTip.SetTip(allowEdits, "Pre-approve Codex calls to Poima's world tools for this connection.");
            setup.Children.Add(new TextBlock { Text = "Game conversation", FontSize = 20, FontWeight = FontWeight.SemiBold });
            setup.Children.Add(new TextBlock { Text = "Connect with your agent’s sign-in. Start with your idea and agree on what you’ll build.", TextWrapping = TextWrapping.Wrap });
            if (gameplayToolStatus is not null)
                setup.Children.Add(new TextBlock { Text = gameplayToolStatus, TextWrapping = TextWrapping.Wrap });
            setup.Children.Add(provider);
            var settings = new StackPanel { Spacing = 6 };
            settings.Children.Add(executable); settings.Children.Add(agyExecutable); settings.Children.Add(claudeExecutable);
            settings.Children.Add(model); settings.Children.Add(thread);
            var options = new Expander { Header = "Connection settings", Content = settings };
            AutomationProperties.SetName(options, "Connection settings");
            setup.Children.Add(options);
            setup.Children.Add(allowEdits);
            if (gameplayToolContext is not null)
            {
                AutomationProperties.SetName(allowSourceEdits, "Agent allow source edits");
                ToolTip.SetTip(allowSourceEdits, "Pre-approve guarded C# source writes for this Codex connection.");
                setup.Children.Add(allowSourceEdits);
                AutomationProperties.SetName(allowBuilds, "Agent allow gameplay builds");
                ToolTip.SetTip(allowBuilds, "Authorize Codex to run the configured project build tools and attach their outputs for this connection.");
                setup.Children.Add(allowBuilds);
                AutomationProperties.SetName(allowChecks, "Agent allow gameplay checks");
                ToolTip.SetTip(allowChecks, "Authorize Codex to edit the checks used to verify this project's exported game.");
                setup.Children.Add(allowChecks);
            }
            var connection = new WrapPanel { ItemSpacing = 8, LineSpacing = 8 };
            connection.Children.Add(connect); connection.Children.Add(disconnect); connection.Children.Add(getCodex);
            connection.Children.Add(newChat);
            setup.Children.Add(connection);
        }
        else
        {
            setup.Children.Add(new TextBlock { Text = "Build with an agent", FontSize = 20 });
            setup.Children.Add(new TextBlock { Text = "Uses your installed agent’s login. Changes reach the same world as the editor.", TextWrapping = TextWrapping.Wrap });
            setup.Children.Add(provider); setup.Children.Add(executable); setup.Children.Add(agyExecutable);
            setup.Children.Add(claudeExecutable); setup.Children.Add(engine);
            setup.Children.Add(model); setup.Children.Add(thread); setup.Children.Add(allowEdits);
            var connection = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            connection.Children.Add(connect); connection.Children.Add(disconnect); connection.Children.Add(getCodex);
            setup.Children.Add(connection);
        }
        root.Children.Add(setup); Grid.SetRow(status, 1); root.Children.Add(status);
        Grid.SetRow(transcript, 2); root.Children.Add(transcript);
        approvalScroll = new ScrollViewer { Content = approvals, MaxHeight = 200 };
        Grid.SetRow(approvalScroll, 3); root.Children.Add(approvalScroll);
        var composer = new StackPanel { Spacing = 6, Children = { imageChips, prompt } };
        Grid.SetRow(composer, 4); root.Children.Add(composer);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(send); actions.Children.Add(stop); actions.Children.Add(addImages);
        Grid.SetRow(actions, 5); root.Children.Add(actions);
        Content = root;
        AutomationProperties.SetName(prompt, "Agent prompt"); AutomationProperties.SetName(transcript, "Agent transcript");
        AutomationProperties.SetName(connect, "Agent Connect"); AutomationProperties.SetName(send, "Agent Send"); AutomationProperties.SetName(stop, "Agent Stop");
        AutomationProperties.SetName(disconnect, "Agent Disconnect"); AutomationProperties.SetName(status, "Agent status");
        AutomationProperties.SetName(allowEdits, "Agent allow world edits");
        ToolTip.SetTip(addImages, "Send PNG or JPEG references with your next message.");
        connect.Click += async (_, _) => await (connectingTask = Connect());
        newChat.Click += async (_, _) =>
        {
            if (!newChat.IsEnabled) return;
            thread.Text = "";
            await (connectingTask = Connect());
        };
        disconnect.Click += async (_, _) =>
        {
            if (session is { } current && !Busy) await Retire(current, "Disconnected.");
        };
        send.Click += async (_, _) => await Send();
        addImages.Click += async (_, _) => await PickImages();
        stop.Click += async (_, _) => await Stop();
        getCodex.Click += async (_, _) => await (installingTask = InstallCodex());
        provider.SelectionChanged += (_, _) => UpdateProvider();
        thread.TextChanged += (_, _) => UpdateConversationButtons();
        UpdateProvider();
    }

    private void UpdateProvider()
    {
        executable.IsVisible = model.IsVisible = thread.IsVisible = allowEdits.IsVisible = ProviderName == "Codex";
        allowSourceEdits.IsVisible = allowBuilds.IsVisible = allowChecks.IsVisible = ProviderName == "Codex";
        agyExecutable.IsVisible = ProviderName == "Antigravity";
        claudeExecutable.IsVisible = ProviderName == "Claude Code";
        getCodex.IsVisible = codexMissing && ProviderName == "Codex";
        UpdateConversationButtons();
    }

    private void UpdateConversationButtons()
    {
        var canContinue = creator && ProviderName == "Codex" && ResumeThread is not null;
        connect.Content = canContinue ? "Continue Codex chat" : "Connect " + ProviderName;
        newChat.IsVisible = canContinue && session is null;
        newChat.IsEnabled = newChat.IsVisible && connect.IsEnabled;
    }

    private static string FindAntigravity()
    {
        var installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "agy", "bin", "agy.exe");
        return File.Exists(installed) ? installed : "agy.exe";
    }

    private static string FindClaudeAdapter()
    {
        var installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Poima", "agents", "claude", "claude-agent-acp.exe");
        return File.Exists(installed) ? installed : "claude-agent-acp.exe";
    }

    private static string FindCodex()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var root = Path.Combine(local, "OpenAI", "Codex", "bin");
        try
        {
            if (Directory.Exists(root))
            {
                var installed = new DirectoryInfo(root).EnumerateDirectories().Take(32)
                    .OrderByDescending(item => item.LastWriteTimeUtc)
                    .Select(item => Path.Combine(item.FullName, "codex.exe"))
                    .FirstOrDefault(File.Exists);
                if (installed is not null) return installed;
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        var standalone = Path.Combine(local, "Programs", "OpenAI", "Codex", "bin", "codex.exe");
        return File.Exists(standalone) ? standalone : WindowsCodexInstall.Executable(WindowsCodexInstall.Root) ?? "codex.exe";
    }

    public Task CloseAsync() => closingTask ??= CloseCoreAsync();

    internal void AddAssetReferences(IReadOnlyList<CreatorAsset> assets)
    {
        var references = string.Join('\n', assets.Select(asset =>
            $"- {asset.Description} {JsonSerializer.Serialize(asset.Name)}: `{asset.Id}`"));
        var draft = prompt.Text;
        prompt.Text = (string.IsNullOrWhiteSpace(draft) ? "" : draft.TrimEnd() + "\n\n") +
            "Imported Poima assets available in this project:\n" + references;
        prompt.CaretIndex = prompt.Text.Length;
        prompt.Focus();
    }
    private async Task CloseCoreAsync()
    {
        closing = true; Controls(); lifetime.Cancel();
        codexInstall?.Stop();
        if (signIn is { } dialog) await dialog.CancelAsync();
        if (installingTask is not null) await installingTask;
        if (connectingTask is not null) await connectingTask;
        if (session is { } current) await DisposeSession(current);
        if (retiringTask is not null) await retiringTask;
    }
    private static TextBox Field(string name, string value)
    {
        var box = new TextBox { Text = value, PlaceholderText = name };
        ToolTip.SetTip(box, name); AutomationProperties.SetName(box, name); return box;
    }
    private void Append(string text)
    {
        var value = transcript.Text + text;
        transcript.Text = value.Length > 100000 ? value[^100000..] : value;
        transcript.CaretIndex = transcript.Text.Length;
    }
    private void Controls()
    {
        var live = ready && session?.ConversationId is not null && !closing;
        send.IsEnabled = live && activeTurn is null && !submitting && !pickingImages;
        var imageSupport = session is AcpSession acp ? acp.SupportsImages : session is CodexSession && codexSupportsImages;
        addImages.IsVisible = live && imageSupport;
        addImages.IsEnabled = addImages.IsVisible && send.IsEnabled && images.Count < AgentImage.MaximumCount;
        imageChips.IsEnabled = send.IsEnabled;
        prompt.IsReadOnly = submitting;
        stop.IsEnabled = live && !cancelRequested && (activeTurn is not null || session is AcpSession && submitting);
        connect.IsEnabled = !Busy && session is null && !closing;
        getCodex.IsEnabled = connect.IsEnabled;
        disconnect.IsEnabled = live && !Busy;
        provider.IsEnabled = executable.IsEnabled = agyExecutable.IsEnabled = claudeExecutable.IsEnabled =
            engine.IsEnabled = model.IsEnabled = connect.IsEnabled;
        thread.IsReadOnly = !connect.IsEnabled; thread.IsEnabled = !closing;
        allowEdits.IsEnabled = allowSourceEdits.IsEnabled = allowBuilds.IsEnabled = allowChecks.IsEnabled = connect.IsEnabled;
        UpdateConversationButtons();
    }
    private async Task Connect()
    {
        connecting = true; Controls();
        status.Text = "Connecting…";
        historyUnavailable = false;
        if (ProviderName == "Codex" && executable.Text == "codex.exe") executable.Text = FindCodex();
        codexMissing = getCodex.IsVisible = false;
        IAgentSession? current = null;
        using var startup = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        startup.CancelAfter(TimeSpan.FromSeconds(60));
        try
        {
            current = ProviderName switch
            {
                "Antigravity" => await AcpSession.ConnectAsync(agyExecutable.Text ?? "", engine.Text ?? "",
                    endpoint, projectRoot, startup.Token),
                "Claude Code" => await AcpSession.ConnectAsync(claudeExecutable.Text ?? "", [], engine.Text ?? "",
                    endpoint, projectRoot, startup.Token),
                _ => await CodexSession.ConnectAsync(executable.Text ?? "", engine.Text ?? "",
                    endpoint, projectRoot, startup.Token, allowEdits.IsChecked == true, allowSourceEdits.IsChecked == true,
                    allowBuilds.IsChecked == true, allowChecks.IsChecked == true, receiveQuestions: true)
            };
            session = current;
            if (closing) { await current.DisposeAsync(); return; }
            _ = ReadEvents(current);
            if (current is AcpSession acp) await acp.OpenAsync(startup.Token);
            else
            {
                var codex = (CodexSession)current;
                if (await codex.RequiresSignInAsync(startup.Token))
                {
                    startup.CancelAfter(Timeout.InfiniteTimeSpan);
                    await SignInAsync(codex, startup.Token);
                    startup.CancelAfter(TimeSpan.FromSeconds(60));
                    if (await codex.RequiresSignInAsync(startup.Token))
                        throw new InvalidOperationException("Codex still needs sign-in. Connect to try again.");
                }
                await OpenCodex(codex, startup.Token);
            }
            if (!ReferenceEquals(session, current)) throw new IOException("Connection ended while opening the conversation.");
            if (current is AcpSession && !string.IsNullOrEmpty(transcript.Text))
                Append("\nNew " + ProviderName + " conversation\n");
            if (current is AcpSession) displayedThread = null;
            ready = true;
            sentGameplayContext = false;
            status.Text = historyUnavailable ? "Codex resumed the chat. Earlier messages couldn’t be displayed." : "Connected to " + ProviderName + ".";
            Controls();
        }
        catch (Exception error)
        {
            if (current is not null) await DisposeSession(current);
            if (ReferenceEquals(session, current)) { session = null; ready = false; }
            if (!closing)
            {
                codexMissing = ProviderName == "Codex" && error is System.ComponentModel.Win32Exception { NativeErrorCode: 2 or 3 };
                getCodex.IsVisible = codexMissing;
                status.Text = error switch
                {
                    _ when codexMissing => "Poima couldn’t find Codex. Install it or choose its executable in Connection settings.",
                    AgentRpcException problem when current is AcpSession &&
                        problem.Error["code"]?.GetValue<int>() == -32000 &&
                        problem.Message.Contains("auth", StringComparison.OrdinalIgnoreCase) =>
                        ProviderName == "Claude Code" ? "Sign in with claude in a terminal, then connect again."
                            : "Sign in with agy in a terminal, then connect again.",
                    System.ComponentModel.Win32Exception problem when ProviderName == "Claude Code" && problem.NativeErrorCode == 2 =>
                        "Install the Claude ACP adapter and select its executable in the connection settings.",
                    _ => error.Message
                };
            }
        }
        finally { connecting = false; Controls(); }
    }

    private async Task InstallCodex()
    {
        if (!getCodex.IsEnabled || !getCodex.IsVisible) return;
        if (!WindowsCodexInstall.Supported)
        {
            try { CodexInstallWindow.OpenGuide(); }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
            { status.Text = "Windows couldn’t open the Codex setup guide."; }
            return;
        }
        if (TopLevel.GetTopLevel(this) is not Window owner) return;
        var dialog = codexInstall = new CodexInstallWindow();
        Controls();
        string? installed;
        try { installed = await dialog.ShowDialog<string?>(owner); }
        finally { codexInstall = null; Controls(); }
        if (installed is null || closing) return;
        executable.Text = installed;
        await (connectingTask = Connect());
    }

    private async Task SignInAsync(CodexSession codex, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        if (TopLevel.GetTopLevel(this) is not Window owner) throw new InvalidOperationException("Open Codex sign-in from a Poima window.");
        var dialog = signIn = new CodexSignInWindow(codex);
        try
        {
            if (!await dialog.ShowDialog<bool>(owner))
                throw new InvalidOperationException("Connect to try Codex sign-in again.");
        }
        finally { signIn = null; }
    }

    private async Task OpenCodex(CodexSession current, CancellationToken cancellation)
    {
        var catalog = await current.ListModelsAsync(cancellation);
        var available = (catalog?["data"] as JsonArray ?? throw new IOException("Codex returned no model catalog."))
            .OfType<JsonObject>().Where(item => item["hidden"]?.GetValue<bool>() != true).ToArray();
        var chosen = string.IsNullOrWhiteSpace(model.Text)
            ? available.FirstOrDefault(item => item["isDefault"]?.GetValue<bool>() == true)?["model"]?.GetValue<string>()
            : model.Text.Trim();
        if (string.IsNullOrWhiteSpace(chosen) || !available.Any(item => item["model"]?.GetValue<string>() == chosen))
            throw new InvalidOperationException("Choose a model offered by this Codex CLI: " + string.Join(", ", available.Select(item => item["model"]?.GetValue<string>())));
        model.Text = chosen;
        codexSupportsImages = available.First(item => item["model"]?.GetValue<string>() == chosen)["inputModalities"] is JsonArray modalities &&
            modalities.OfType<JsonValue>().Any(value => value.TryGetValue<string>(out var modality) && modality == "image");
        ToolTip.SetTip(model, "Models offered by this CLI: " + string.Join(", ", available.Select(item => item["model"]?.GetValue<string>())));
        // An unused conversation does not yet have provider history to resume.
        var resume = ResumeThread;
        if (resume is not null && resume == rememberedThread)
        {
            var saved = (await current.ReadThreadAsync(resume, cancellation))?["thread"];
            var folder = saved?["cwd"]?.GetValue<string>();
            var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (saved?["id"]?.GetValue<string>() != resume || folder is null || !Path.IsPathFullyQualified(folder) ||
                !string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder)),
                    Path.TrimEndingDirectorySeparator(Path.GetFullPath(projectRoot)), comparison))
                throw new InvalidOperationException("The saved Codex chat belongs to a different folder. Choose New chat to continue here.");
        }
        var id = await current.OpenAsync(resume, cancellation, chosen);
        unusedConversation = resume is null ? id : null;
        thread.Text = id;
        if (conversationStore is not null && displayedThread != id)
        {
            transcript.Text = "";
            displayedThread = id;
            if (resume is not null) await ShowRecentMessages(current, cancellation);
        }
    }

    private async Task ShowRecentMessages(CodexSession current, CancellationToken cancellation)
    {
        try
        {
            var recent = new List<JsonObject>();
            string? cursor = null;
            for (var page = 0; page < 3; page++)
            {
                var reply = await current.ListTurnsAsync(cursor, cancellation, limit: 1);
                if (reply?["data"] is not JsonArray turns || turns.Count > 1)
                    throw new InvalidDataException("Invalid Codex conversation page.");
                if (turns.Count == 0) break;
                if (turns[0] is not JsonObject turn) throw new InvalidDataException("Invalid Codex conversation turn.");
                recent.Add(turn);
                var next = reply["nextCursor"]?.GetValue<string>();
                if (string.IsNullOrEmpty(next) || next == cursor) break;
                cursor = next;
            }
            for (var page = recent.Count - 1; page >= 0; page--)
            {
                if (recent[page]["items"] is not JsonArray items) continue;
                foreach (var item in items.OfType<JsonObject>())
                {
                    var kind = item["type"]?.GetValue<string>();
                    if (kind == "agentMessage" && item["text"]?.GetValue<string>() is { Length: > 0 } reply)
                        Append("\nCodex: " + reply + "\n");
                    if (kind != "userMessage" || item["content"] is not JsonArray blocks) continue;
                    var text = string.Join("\n", blocks.OfType<JsonObject>().Select(block => block["type"]?.GetValue<string>() switch
                    {
                        "text" => block["text"]?.GetValue<string>(),
                        "image" or "localImage" => "[Image reference]",
                        _ => null
                    }).OfType<string>());
                    if (gameplayToolContext is not null && text.EndsWith("\n\n" + gameplayToolContext, StringComparison.Ordinal))
                        text = text[..^(gameplayToolContext.Length + 2)];
                    if (text.Length > 0) Append("\nYou: " + text + "\n");
                }
            }
        }
        catch (Exception error) when (error is AgentRpcException or AgentExchangeException or InvalidOperationException or InvalidDataException)
        { historyUnavailable = true; }
    }

    private void RememberCompletedChat(string? id)
    {
        if (conversationStore is null || id is null || id == rememberedThread) return;
        try
        {
            conversationStore.Write(id);
            rememberedThread = id;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException)
        { status.Text = "Turn completed. Poima couldn’t save the chat for reopening."; }
    }

    private async Task PickImages()
    {
        if (!addImages.IsEnabled || session is not { } connection || TopLevel.GetTopLevel(this) is not { } window) return;
        pickingImages = true; Controls();
        try
        {
            var files = await window.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = "Choose reference images", AllowMultiple = AgentImage.MaximumCount - images.Count > 1,
                FileTypeFilter = [new FilePickerFileType("PNG and JPEG images") { Patterns = ["*.png", "*.jpg", "*.jpeg"] }]
            });
            try
            {
                if (closing || !ReferenceEquals(session, connection) || files.Count == 0) return;
                if (files.Count + images.Count > AgentImage.MaximumCount)
                    throw new InvalidDataException("Attach up to three images per message.");
                var selected = new List<(string Name, AgentImage Image)>();
                foreach (var file in files)
                    selected.Add((file.Name, await ReferenceImage.ReadAsync(file, lifetime.Token)));
                if (closing || !ReferenceEquals(session, connection)) return;
                foreach (var item in selected) AddImage(item.Name, item.Image);
                status.Text = images.Count == 1 ? "One image ready to send." : $"{images.Count} images ready to send.";
                prompt.Focus();
            }
            finally { foreach (var file in files) file.Dispose(); }
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or InvalidOperationException or OperationCanceledException)
        {
            if (!closing && ReferenceEquals(session, connection)) status.Text = error.Message;
        }
        finally { pickingImages = false; Controls(); }
    }

    private void AddImage(string name, AgentImage image)
    {
        var chip = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 6 };
        var label = new TextBlock { Text = name, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
        ToolTip.SetTip(label, name);
        var remove = new Button { Content = "×" };
        AutomationProperties.SetName(remove, "Remove image " + name);
        Grid.SetColumn(remove, 1);
        chip.Children.Add(label);
        chip.Children.Add(remove);
        var entry = (name, image, (Control)chip);
        remove.Click += (_, _) =>
        {
            images.Remove(entry);
            imageChips.Children.Remove(chip);
            imageChips.IsVisible = images.Count != 0;
            status.Text = "Image removed.";
            Controls();
        };
        images.Add(entry);
        imageChips.Children.Add(chip);
        imageChips.IsVisible = true;
    }

    private void ClearImages()
    {
        images.Clear();
        imageChips.Children.Clear();
        imageChips.IsVisible = false;
    }

    private async Task Send()
    {
        if (!send.IsEnabled || session is not { } current || string.IsNullOrWhiteSpace(prompt.Text)) return;
        completed.Clear();
        lastSuccessfulCodexTurn = null;
        var text = prompt.Text; submitting = true; cancelRequested = false; Controls();
        var attachments = images.Select(entry => entry.Image).ToArray();
        var names = images.Count == 0 ? "" : "\nImages: " + string.Join(", ", images.Select(entry => entry.Name));
        var payload = !sentGameplayContext && gameplayToolContext is not null ? text + "\n\n" + gameplayToolContext : text;
        Append("\nYou: " + text + names + "\n\n" + ProviderName + ": "); status.Text = "Working…";
        try
        {
            if (current is AcpSession acp)
            {
                acpReceivedReply = false;
                var reason = await acp.PromptAsync(payload, attachments, lifetime.Token);
                if (ReferenceEquals(session, current))
                {
                    sentGameplayContext = true;
                    prompt.Text = ""; ClearImages();
                    status.Text = reason switch
                    {
                        "end_turn" => acpReceivedReply ? "Turn completed" : ProviderName + " ended the turn without a reply.",
                        "cancelled" => "Turn cancelled",
                        _ => ProviderName + " ended the turn: " + reason
                    };
                    Append("\n");
                }
                return;
            }
            var id = await ((CodexSession)current).SendAsync(payload, attachments, lifetime.Token);
            if (ReferenceEquals(session, current))
            {
                sentGameplayContext = true;
                prompt.Text = ""; ClearImages();
                unusedConversation = null;
                if (!completed.Contains(id)) activeTurn = submittedCodexTurn = id;
                else if (lastSuccessfulCodexTurn == id) RememberCompletedChat(current.ConversationId);
            }
        }
        catch (Exception error)
        {
            if (ReferenceEquals(session, current))
            {
                status.Text = error.Message;
                if (error is AgentExchangeException { OutcomeUnknown: true })
                {
                    if (current is CodexSession) unusedConversation = null;
                    await Retire(current, error.Message);
                }
            }
        }
        finally
        {
            if (ReferenceEquals(session, current))
            {
                submitting = false;
                if (current is AcpSession) ClearApprovals();
            }
            Controls();
        }
    }
    private async Task Stop()
    {
        if (session is not { } current || !stop.IsEnabled) return;
        var id = activeTurn;
        cancelRequested = true; Controls();
        if (questionCard is { } pendingQuestion) pendingQuestion.IsEnabled = false;
        foreach (var card in codexPoimaApprovals.Keys) card.IsEnabled = false;
        try
        {
            if (current is AcpSession acp)
            {
                var pending = acpPermissions.Values.ToArray();
                ClearApprovals();
                await acp.CancelAsync(lifetime.Token);
                foreach (var request in pending) await Reply(current, request, PermissionOutcome(null));
            }
            else await ((CodexSession)current).InterruptAsync(id!, lifetime.Token);
            if (ReferenceEquals(session, current) && (current is AcpSession ? submitting : activeTurn == id))
                status.Text = "Stop requested. Waiting for the turn to finish. Existing edits remain.";
        }
        catch (Exception error)
        {
            if (ReferenceEquals(session, current))
            {
                cancelRequested = false;
                if (questionCard is { } pending) pending.IsEnabled = true;
                foreach (var card in codexPoimaApprovals.Keys) card.IsEnabled = true;
                status.Text = error.Message; Controls();
            }
        }
    }
    private async Task ReadEvents(IAgentSession current)
    {
        try
        {
            await foreach (var message in current.Events.ReadAllAsync(lifetime.Token).ConfigureAwait(false))
            {
                await Dispatcher.UIThread.InvokeAsync(() => Handle(current, message));
                if (current is AcpSession acp) acp.AcknowledgeEvent();
            }
        }
        catch (Exception error)
        {
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                if (!closing && ReferenceEquals(session, current))
                    _ = Retire(current, "Connection ended: " + error.Message);
            });
        }
    }
    private async Task Retire(IAgentSession current, string message)
    {
        if (!ReferenceEquals(session, current)) return;
        session = null; ready = false; activeTurn = null; submitting = false; cancelRequested = false;
        submittedCodexTurn = null;
        ClearApprovals(); ClearImages(); status.Text = message;
        if (signIn is { } dialog) await dialog.CancelAsync();
        var pending = retiringTask = DisposeSession(current);
        Controls();
        await pending;
        Controls();
    }
    private async Task DisposeSession(IAgentSession current)
    {
        try { await current.DisposeAsync(); }
        catch (Exception error) { if (!closing) status.Text = "Agent shutdown failed: " + error.Message; }
    }
    private void Handle(IAgentSession current, JsonObject message)
    {
        if (closing || !ReferenceEquals(session, current)) return;
        var method = message["method"]?.GetValue<string>() ?? "";
        var parameters = message["params"] as JsonObject;
        if (current is AcpSession)
        {
            HandleAcp(current, method, parameters, message["id"]);
            return;
        }
        if (method == "account/login/completed" && message["id"] is null)
        {
            signIn?.ObserveCompletion(parameters);
            return;
        }
        if (message["id"] is JsonNode requestId)
        {
            if (method == "item/tool/requestUserInput")
            {
                if (!ShowQuestions(current, requestId, parameters)) _ = RejectUnsupported(current, requestId, method);
            }
            else if (method == "mcpServer/elicitation/request")
            {
                if (!cancelRequested && approvals.Children.Count < 16 &&
                    CodexPoimaApproval.Read(parameters, current.ConversationId, activeTurn) is { } approval)
                {
                    ShowApproval(current, requestId, approval.Details,
                        new JsonObject { ["action"] = "accept", ["content"] = new JsonObject() },
                        new JsonObject { ["action"] = "decline" }, approval.Title);
                }
                else _ = Reply(current, requestId, new JsonObject { ["action"] = "cancel" });
            }
            else if (method is "item/commandExecution/requestApproval" or "item/fileChange/requestApproval" && approvals.Children.Count < 16)
            {
                ShowApproval(current, requestId, method + "\n" + parameters?.ToJsonString(),
                    new JsonObject { ["decision"] = "accept" }, new JsonObject { ["decision"] = "decline" });
            }
            else _ = RejectUnsupported(current, requestId, method);
            return;
        }
        if (parameters?["threadId"]?.GetValue<string>() != current.ConversationId) return;
        if (method == "item/agentMessage/delta") Append(parameters?["delta"]?.GetValue<string>() ?? "");
        else if (method == "serverRequest/resolved" && questionCard is { } card &&
            JsonNode.DeepEquals(card.RequestId, parameters?["requestId"]))
        {
            RemoveQuestions(card);
            if (!cancelRequested) status.Text = "Working…";
        }
        else if (method == "serverRequest/resolved")
        {
            var approval = codexPoimaApprovals.FirstOrDefault(entry => JsonNode.DeepEquals(entry.Value, parameters?["requestId"])).Key;
            if (approval is not null) RemoveApproval(approval);
        }
        else if (method == "turn/started") { activeTurn = parameters?["turn"]?["id"]?.GetValue<string>(); Controls(); }
        else if (method == "turn/completed")
        {
            var id = parameters?["turn"]?["id"]?.GetValue<string>();
            if (id is not null) completed.Add(id);
            if (activeTurn == id) activeTurn = null;
            status.Text = "Turn " + parameters?["turn"]?["status"] + (parameters?["turn"]?["error"] is { } error ? ": " + error.ToJsonString() : "");
            if (id is not null && parameters?["turn"]?["status"]?.GetValue<string>() == "completed")
            {
                lastSuccessfulCodexTurn = id;
                if (id == submittedCodexTurn) RememberCompletedChat(current.ConversationId);
            }
            if (id == submittedCodexTurn) submittedCodexTurn = null;
            ClearApprovals(); Append("\n"); Controls();
        }
        else if (method == "error") status.Text = parameters?.ToJsonString();
    }

    private bool ShowQuestions(IAgentSession current, JsonNode requestId, JsonObject? parameters)
    {
        if (questionCard is not null || cancelRequested || !Busy ||
            parameters?["threadId"] is not JsonValue identity || !identity.TryGetValue<string>(out var threadId) ||
            threadId != current.ConversationId || CodexQuestionCard.Create(requestId, parameters) is not { } card ||
            activeTurn is { } turn && turn != card.TurnId || completed.Contains(card.TurnId)) return false;
        questionCard = card;
        approvals.Children.Add(card);
        approvalScroll.MaxHeight = 290;
        prompt.IsVisible = false;
        status.Text = "Waiting for your answers.";
        card.OnSubmit(async answer =>
        {
            if (!ReferenceEquals(questionCard, card) || !ReferenceEquals(session, current) || closing || cancelRequested) return;
            RemoveQuestions(card);
            status.Text = "Working…";
            await Reply(current, card.RequestId, answer);
        });
        return true;
    }

    private void RemoveQuestions(CodexQuestionCard card)
    {
        card.IsEnabled = false;
        approvals.Children.Remove(card);
        questionCard = null;
        approvalScroll.MaxHeight = 200;
        prompt.IsVisible = true;
    }
    private void HandleAcp(IAgentSession current, string method, JsonObject? parameters, JsonNode? id)
    {
        var matches = current.ConversationId is { } conversation && parameters?["sessionId"]?.GetValue<string>() == conversation;
        if (id is not null)
        {
            if (method != "session/request_permission")
            {
                _ = RejectUnsupported(current, id, method);
                return;
            }
            var options = (parameters?["options"] as JsonArray)?.OfType<JsonObject>() ?? [];
            string? Option(string kind) => options.FirstOrDefault(option => option["kind"]?.GetValue<string>() == kind)?["optionId"]?.GetValue<string>();
            var allow = Option("allow_once");
            var deny = PermissionOutcome(Option("reject_once"));
            if (matches && submitting && !cancelRequested && approvals.Children.Count < 16 && !string.IsNullOrEmpty(allow))
            {
                var detail = DescribeAcpTool(parameters?["toolCall"] as JsonObject);
                if (detail is { Length: > 0 and <= 16384 })
                    ShowApproval(current, id, detail, PermissionOutcome(allow), deny);
                else
                {
                    status.Text = ProviderName + " requested permission without reviewable tool details.";
                    _ = Reply(current, id, deny);
                }
            }
            else _ = Reply(current, id, matches && submitting && !cancelRequested ? deny : PermissionOutcome(null));
        }
        else if (matches && method == "session/update" && parameters?["update"] is JsonObject update)
        {
            var kind = update["sessionUpdate"]?.GetValue<string>();
            if (kind == "agent_message_chunk" &&
                update["content"] is JsonObject content && content["type"]?.GetValue<string>() == "text")
            {
                var text = content["text"]?.GetValue<string>() ?? "";
                if (!string.IsNullOrWhiteSpace(text)) acpReceivedReply = true;
                Append(text);
            }
            else if (kind is "tool_call" or "tool_call_update" && submitting &&
                update["toolCallId"]?.GetValue<string>() is { } toolId)
            {
                if (update["status"]?.GetValue<string>() is "completed" or "failed") acpToolDetails.Remove(toolId);
                else if (acpToolDetails.Count < 128 && DescribeAcpTool(update) is { Length: > 0 and <= 16384 } detail)
                    acpToolDetails[toolId] = detail;
            }
        }
    }

    private string DescribeAcpTool(JsonObject? tool)
    {
        if (tool is null) return "";
        var detail = string.Join("\n", new[]
        {
            tool["title"]?.GetValue<string>(),
            tool["rawInput"]?.ToJsonString(),
            tool["locations"]?.ToJsonString(),
            tool["content"]?.ToJsonString()
        }.Where(text => !string.IsNullOrWhiteSpace(text)));
        var previous = tool["toolCallId"]?.GetValue<string>() is { } id ? acpToolDetails.GetValueOrDefault(id) : null;
        if (string.IsNullOrEmpty(previous) || detail.Contains(previous, StringComparison.Ordinal)) return detail;
        if (previous.Contains(detail, StringComparison.Ordinal)) return previous;
        return previous + "\n" + detail;
    }

    private static JsonObject PermissionOutcome(string? option) => new()
    {
        ["outcome"] = option is null ? new JsonObject { ["outcome"] = "cancelled" }
            : new JsonObject { ["outcome"] = "selected", ["optionId"] = option }
    };

    private void ShowApproval(IAgentSession current, JsonNode id, string message, JsonObject allow, JsonObject deny, string? poimaTitle = null)
    {
        var card = new StackPanel { Spacing = 4 };
        if (poimaTitle is not null)
        {
            card.Children.Add(new TextBlock { Text = poimaTitle, TextWrapping = TextWrapping.Wrap, FontWeight = FontWeight.SemiBold });
            codexPoimaApprovals.Add(card, id.DeepClone());
            status.Text = "Waiting for your approval.";
        }
        var details = new TextBox { Text = message, IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MaxHeight = 130 };
        AutomationProperties.SetName(details, "Agent approval details");
        card.Children.Add(details);
        if (current is AcpSession) acpPermissions.Add(card, id);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (var (label, reply) in new[] { ("Approve once", allow), ("Decline", deny) })
        {
            var button = new Button { Content = label };
            button.Click += async (_, _) =>
            {
                if (!ReferenceEquals(session, current) || closing || !card.IsEnabled ||
                    (current is AcpSession || poimaTitle is not null) && cancelRequested) return;
                RemoveApproval(card);
                await Reply(current, id, reply);
            };
            buttons.Children.Add(button);
        }
        card.Children.Add(buttons); approvals.Children.Add(card);
    }

    private void RemoveApproval(StackPanel card)
    {
        card.IsEnabled = false;
        acpPermissions.Remove(card);
        if (codexPoimaApprovals.Remove(card) && codexPoimaApprovals.Count == 0 && !cancelRequested)
            status.Text = "Working…";
        approvals.Children.Remove(card);
    }

    private void ClearApprovals()
    {
        if (questionCard is { } card) RemoveQuestions(card);
        foreach (var child in approvals.Children) child.IsEnabled = false;
        approvals.Children.Clear();
        acpPermissions.Clear();
        codexPoimaApprovals.Clear();
        acpToolDetails.Clear();
    }

    private async Task Reply(IAgentSession current, JsonNode id, JsonObject reply)
    {
        try { await current.ReplyAsync(id, reply, lifetime.Token); }
        catch (Exception error) { if (!closing && ReferenceEquals(session, current)) status.Text = error.Message; }
    }

    private async Task RejectUnsupported(IAgentSession current, JsonNode id, string method)
    {
        try { await current.RejectAsync(id, "This Poima client does not yet support " + method, lifetime.Token); status.Text = "Unsupported request declined: " + method; }
        catch (Exception error) { if (!closing) status.Text = error.Message; }
    }
    internal JsonObject InspectForQualification() => new()
    {
        ["ready"] = ready, ["connecting"] = connecting, ["send_enabled"] = send.IsEnabled,
        ["stop_enabled"] = stop.IsEnabled, ["connect_enabled"] = connect.IsEnabled,
        ["thread_id"] = session?.ConversationId, ["turn_id"] = activeTurn,
        ["status"] = status.Text, ["transcript"] = transcript.Text,
        ["approvals"] = approvals.Children.Count
    };
}

public sealed class AgentWindow : Window
{
    private readonly AgentChat chat;
    private readonly string projectRoot;
    public AgentWindow(string projectRoot, string endpoint)
    {
        this.projectRoot = projectRoot;
        Title = "Agent — Poima"; Width = 760; Height = 760; MinWidth = 440; MinHeight = 480;
        Background = EditorTheme.Brush("#222327");
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Content = chat = new AgentChat(projectRoot, endpoint);
        Closing += async (_, _) => await chat.CloseAsync();
    }
    internal JsonObject InspectForQualification() => chat.InspectForQualification();
    internal JsonObject RenderForQualification(string destination)
    {
        if (Program.Options.Script is null || !IsVisible || !IsMeasureValid || !IsArrangeValid)
            throw new InvalidOperationException("Agent capture requires a visible script-owned window.");
        var path = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (path.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(path).ToLowerInvariant() != ".png")
            throw new ArgumentException("Agent capture must be a new PNG outside the project directory.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(path)!);
        if (!parent.Exists || File.Exists(path) || Directory.Exists(path)) throw new ArgumentException("Agent capture needs a new file in an existing directory.");
        for (var check = parent; check is not null; check = check.Parent)
            if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Agent capture cannot traverse a reparse point.");
        var width = Math.Ceiling(Bounds.Width * RenderScaling); var height = Math.Ceiling(Bounds.Height * RenderScaling);
        if (!double.IsFinite(width) || !double.IsFinite(height) || width is < 32 or > 4096 || height is < 32 or > 4096 || width * height > 16777216)
            throw new InvalidOperationException("Agent capture exceeds the dimension budget.");
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(this);
        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        image.Save(output); output.Flush(true);
        return new JsonObject { ["kind"] = "avalonia_visual_content", ["path"] = path, ["width"] = (int)width,
            ["height"] = (int)height, ["scale"] = RenderScaling, ["qualification"] = "Attached Agent client visual; not an OS screenshot or physical-input qualification." };
    }
}
