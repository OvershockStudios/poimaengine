// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Poima.AgentHost;
using System.Text.Json.Nodes;

namespace Poima.Editor;

/// <summary>Chat client over the shared engine endpoint; never calls NativeHost from a worker.</summary>
internal sealed class AgentChat : UserControl
{
    private readonly string projectRoot, endpoint;
    private readonly bool creator;
    private readonly CancellationTokenSource lifetime = new();
    private readonly ComboBox provider = new() { ItemsSource = new[] { "Codex", "Antigravity" }, SelectedIndex = 0 };
    private readonly TextBox executable = Field("Codex executable", "codex.exe");
    private readonly TextBox agyExecutable = Field("Antigravity CLI executable", FindAntigravity());
    private readonly TextBox engine = Field("Poima executable", Path.Combine(AppContext.BaseDirectory, "poima.exe"));
    private readonly TextBox thread = Field("Conversation ID (empty starts a new conversation)", "");
    private readonly TextBox model = Field("Model (empty uses the CLI's advertised default)", "");
    private readonly CheckBox allowEdits = new() { Content = "Allow Poima world edits in this session", IsChecked = true };
    private readonly TextBox prompt = new() { AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 85, MaxHeight = 180 };
    private readonly TextBox transcript = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock status = new() { Text = "Disconnected", TextWrapping = TextWrapping.Wrap };
    private readonly StackPanel approvals = new() { Spacing = 6 };
    private readonly Button connect = new() { Content = "Connect" }, disconnect = new() { Content = "Disconnect", IsEnabled = false },
        send = new() { Content = "Send", IsEnabled = false }, stop = new() { Content = "Stop", IsEnabled = false };
    private IAgentSession? session;
    private string? activeTurn, unusedConversation;
    private readonly HashSet<string> completed = new(StringComparer.Ordinal);
    private readonly Dictionary<StackPanel, JsonNode> acpPermissions = [];
    private readonly Dictionary<string, string> acpToolDetails = new(StringComparer.Ordinal);
    private bool submitting, closing, connecting, ready, cancelRequested;
    private Task? connectingTask, closingTask, retiringTask;
    public bool Busy => submitting || activeTurn is not null || connecting || retiringTask?.IsCompleted == false;
    private bool UsesAntigravity => provider.SelectedIndex == 1;
    private string ProviderName => UsesAntigravity ? "Antigravity" : "Codex";
    public AgentChat(string projectRoot, string endpoint, bool creator = false)
    {
        this.projectRoot = projectRoot; this.endpoint = endpoint; this.creator = creator;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = creator ? 14 : 12;
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto,Auto,Auto"), Margin = new Thickness(14), RowSpacing = 8 };
        prompt.PlaceholderText = creator ? "Describe the game you want to make…" : "Ask about this project…";
        var setup = new StackPanel { Spacing = 6 };
        AutomationProperties.SetName(provider, "Agent provider");
        if (creator)
        {
            executable.Text = FindCodex();
            allowEdits.IsChecked = false;
            allowEdits.Content = "Allow Poima tools without asking";
            ToolTip.SetTip(allowEdits, "Pre-approve Codex calls to Poima's world tools for this connection.");
            setup.Children.Add(new TextBlock { Text = "Game conversation", FontSize = 20, FontWeight = FontWeight.SemiBold });
            setup.Children.Add(new TextBlock { Text = "Connect with your agent’s sign-in. Start with your idea and agree on what you’ll build.", TextWrapping = TextWrapping.Wrap });
            setup.Children.Add(provider);
            var settings = new StackPanel { Spacing = 6 };
            settings.Children.Add(executable); settings.Children.Add(agyExecutable); settings.Children.Add(model); settings.Children.Add(thread);
            var options = new Expander { Header = "Connection settings", Content = settings };
            AutomationProperties.SetName(options, "Connection settings");
            setup.Children.Add(options);
            setup.Children.Add(allowEdits);
            var connection = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            connection.Children.Add(connect); connection.Children.Add(disconnect);
            setup.Children.Add(connection);
        }
        else
        {
            setup.Children.Add(new TextBlock { Text = "Build with an agent", FontSize = 20 });
            setup.Children.Add(new TextBlock { Text = "Uses your installed agent’s login. Changes reach the same world as the editor.", TextWrapping = TextWrapping.Wrap });
            setup.Children.Add(provider); setup.Children.Add(executable); setup.Children.Add(agyExecutable); setup.Children.Add(engine);
            setup.Children.Add(model); setup.Children.Add(thread); setup.Children.Add(allowEdits);
            var connection = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            connection.Children.Add(connect); connection.Children.Add(disconnect);
            setup.Children.Add(connection);
        }
        root.Children.Add(setup); Grid.SetRow(status, 1); root.Children.Add(status);
        Grid.SetRow(transcript, 2); root.Children.Add(transcript);
        var approvalScroll = new ScrollViewer { Content = approvals, MaxHeight = 200 };
        Grid.SetRow(approvalScroll, 3); root.Children.Add(approvalScroll);
        Grid.SetRow(prompt, 4); root.Children.Add(prompt);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(send); actions.Children.Add(stop); Grid.SetRow(actions, 5); root.Children.Add(actions);
        Content = root;
        AutomationProperties.SetName(prompt, "Agent prompt"); AutomationProperties.SetName(transcript, "Agent transcript");
        AutomationProperties.SetName(connect, "Agent Connect"); AutomationProperties.SetName(send, "Agent Send"); AutomationProperties.SetName(stop, "Agent Stop");
        AutomationProperties.SetName(disconnect, "Agent Disconnect"); AutomationProperties.SetName(status, "Agent status");
        AutomationProperties.SetName(allowEdits, "Agent allow world edits");
        connect.Click += async (_, _) => await (connectingTask = Connect());
        disconnect.Click += async (_, _) =>
        {
            if (session is { } current && !Busy) await Retire(current, "Disconnected.");
        };
        send.Click += async (_, _) => await Send();
        stop.Click += async (_, _) => await Stop();
        provider.SelectionChanged += (_, _) => UpdateProvider();
        UpdateProvider();
    }

    private void UpdateProvider()
    {
        executable.IsVisible = model.IsVisible = thread.IsVisible = allowEdits.IsVisible = !UsesAntigravity;
        agyExecutable.IsVisible = UsesAntigravity;
        connect.Content = "Connect " + ProviderName;
    }

    private static string FindAntigravity()
    {
        var installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "agy", "bin", "agy.exe");
        return File.Exists(installed) ? installed : "agy.exe";
    }

    private static string FindCodex()
    {
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenAI", "Codex", "bin");
        try
        {
            if (Directory.Exists(root))
                return new DirectoryInfo(root).EnumerateDirectories().Take(32)
                    .OrderByDescending(item => item.LastWriteTimeUtc)
                    .Select(item => Path.Combine(item.FullName, "codex.exe"))
                    .FirstOrDefault(File.Exists) ?? "codex.exe";
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return "codex.exe";
    }

    public Task CloseAsync() => closingTask ??= CloseCoreAsync();
    private async Task CloseCoreAsync()
    {
        closing = true; Controls(); lifetime.Cancel();
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
        send.IsEnabled = live && activeTurn is null && !submitting;
        stop.IsEnabled = live && !cancelRequested && (activeTurn is not null || session is AcpSession && submitting);
        connect.IsEnabled = !Busy && session is null && !closing;
        disconnect.IsEnabled = live && !Busy;
        provider.IsEnabled = executable.IsEnabled = agyExecutable.IsEnabled = engine.IsEnabled = model.IsEnabled = connect.IsEnabled;
        thread.IsReadOnly = !connect.IsEnabled; thread.IsEnabled = !closing;
        allowEdits.IsEnabled = connect.IsEnabled;
    }
    private async Task Connect()
    {
        connecting = true; Controls();
        status.Text = "Connecting…";
        IAgentSession? current = null;
        using var startup = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        startup.CancelAfter(TimeSpan.FromSeconds(60));
        try
        {
            current = UsesAntigravity
                ? await AcpSession.ConnectAsync(agyExecutable.Text ?? "", engine.Text ?? "", endpoint, projectRoot, startup.Token)
                : await CodexSession.ConnectAsync(executable.Text ?? "", engine.Text ?? "", endpoint, projectRoot, startup.Token, allowEdits.IsChecked == true);
            session = current;
            if (closing) { await current.DisposeAsync(); return; }
            _ = ReadEvents(current);
            if (current is AcpSession acp) await acp.OpenAsync(startup.Token);
            else await OpenCodex((CodexSession)current, startup.Token);
            if (!ReferenceEquals(session, current)) throw new IOException("Connection ended while opening the conversation.");
            if (current is AcpSession && !string.IsNullOrEmpty(transcript.Text))
                Append("\nNew Antigravity conversation\n");
            ready = true;
            status.Text = "Connected to " + ProviderName + ".";
            Controls();
        }
        catch (Exception error)
        {
            if (current is not null) await DisposeSession(current);
            if (ReferenceEquals(session, current)) { session = null; ready = false; }
            if (!closing) status.Text = error.Message;
        }
        finally { connecting = false; Controls(); }
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
        ToolTip.SetTip(model, "Models offered by this CLI: " + string.Join(", ", available.Select(item => item["model"]?.GetValue<string>())));
        var requested = thread.Text?.Trim();
        // An unused conversation does not yet have provider history to resume.
        var resume = string.IsNullOrEmpty(requested) || requested == unusedConversation ? null : requested;
        var id = await current.OpenAsync(resume, cancellation, chosen);
        unusedConversation = resume is null ? id : null;
        thread.Text = id;
    }
    private async Task Send()
    {
        if (!ready || session is not { } current || activeTurn is not null || submitting || string.IsNullOrWhiteSpace(prompt.Text)) return;
        completed.Clear();
        var text = prompt.Text; submitting = true; cancelRequested = false; Controls();
        Append("\nYou: " + text + "\n\n" + ProviderName + ": "); prompt.Text = ""; status.Text = "Working…";
        try
        {
            if (current is AcpSession acp)
            {
                var reason = await acp.PromptAsync(text, lifetime.Token);
                if (ReferenceEquals(session, current))
                {
                    status.Text = reason switch
                    {
                        "end_turn" => "Turn completed",
                        "cancelled" => "Turn cancelled",
                        _ => "Antigravity ended the turn: " + reason
                    };
                    Append("\n");
                }
                return;
            }
            var id = await ((CodexSession)current).SendAsync(text, lifetime.Token);
            if (ReferenceEquals(session, current))
            {
                unusedConversation = null;
                if (!completed.Contains(id)) activeTurn = id;
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
            if (ReferenceEquals(session, current)) { cancelRequested = false; status.Text = error.Message; Controls(); }
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
        ClearApprovals(); status.Text = message;
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
            HandleAntigravity(current, method, parameters, message["id"]);
            return;
        }
        if (message["id"] is JsonNode requestId)
        {
            if (method is "item/commandExecution/requestApproval" or "item/fileChange/requestApproval" && approvals.Children.Count < 16)
            {
                ShowApproval(current, requestId, method + "\n" + parameters?.ToJsonString(),
                    new JsonObject { ["decision"] = "accept" }, new JsonObject { ["decision"] = "decline" });
            }
            else _ = RejectUnsupported(current, requestId, method);
            return;
        }
        if (parameters?["threadId"]?.GetValue<string>() != current.ConversationId) return;
        if (method == "item/agentMessage/delta") Append(parameters?["delta"]?.GetValue<string>() ?? "");
        else if (method == "turn/started") { activeTurn = parameters?["turn"]?["id"]?.GetValue<string>(); Controls(); }
        else if (method == "turn/completed")
        {
            var id = parameters?["turn"]?["id"]?.GetValue<string>();
            if (id is not null) completed.Add(id);
            if (activeTurn == id) activeTurn = null;
            status.Text = "Turn " + parameters?["turn"]?["status"] + (parameters?["turn"]?["error"] is { } error ? ": " + error.ToJsonString() : "");
            approvals.Children.Clear(); Append("\n"); Controls();
        }
        else if (method == "error") status.Text = parameters?.ToJsonString();
    }
    private void HandleAntigravity(IAgentSession current, string method, JsonObject? parameters, JsonNode? id)
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
                    status.Text = "Antigravity requested permission without reviewable tool details.";
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
                Append(content["text"]?.GetValue<string>() ?? "");
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

    private void ShowApproval(IAgentSession current, JsonNode id, string message, JsonObject allow, JsonObject deny)
    {
        var card = new StackPanel { Spacing = 4 };
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
                if (!ReferenceEquals(session, current) || closing || !card.IsEnabled || current is AcpSession && cancelRequested) return;
                card.IsEnabled = false;
                acpPermissions.Remove(card);
                await Reply(current, id, reply);
                approvals.Children.Remove(card);
            };
            buttons.Children.Add(button);
        }
        card.Children.Add(buttons); approvals.Children.Add(card);
    }

    private void ClearApprovals()
    {
        foreach (var child in approvals.Children) child.IsEnabled = false;
        approvals.Children.Clear();
        acpPermissions.Clear();
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
