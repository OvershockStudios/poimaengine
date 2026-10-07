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

/// <summary>First chat client over the shared engine endpoint; never calls NativeHost from a worker.</summary>
public sealed class AgentWindow : Window
{
    private readonly string projectRoot, endpoint;
    private readonly CancellationTokenSource lifetime = new();
    private readonly TextBox executable = Field("Codex executable", "codex.exe");
    private readonly TextBox engine = Field("Poima executable", Path.Combine(AppContext.BaseDirectory, "poima.exe"));
    private readonly TextBox thread = Field("Conversation ID (empty starts a new conversation)", "");
    private readonly TextBox model = Field("Model (empty uses the CLI's advertised default)", "");
    private readonly CheckBox allowEdits = new() { Content = "Allow Poima world edits in this session", IsChecked = true };
    private readonly TextBox prompt = new() { AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 85, MaxHeight = 180 };
    private readonly TextBox transcript = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock status = new() { Text = "Disconnected", TextWrapping = TextWrapping.Wrap };
    private readonly StackPanel approvals = new() { Spacing = 6 };
    private readonly Button connect = new() { Content = "Connect" }, send = new() { Content = "Send", IsEnabled = false }, stop = new() { Content = "Stop", IsEnabled = false };
    private CodexSession? session;
    private string? activeTurn;
    private readonly HashSet<string> completed = new(StringComparer.Ordinal);
    private bool submitting, closing, connecting, ready;
    public AgentWindow(string projectRoot, string endpoint)
    {
        this.projectRoot = projectRoot; this.endpoint = endpoint;
        Title = "Agent — Poima"; Width = 760; Height = 760; MinWidth = 440; MinHeight = 480;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = 12;
        Background = EditorTheme.Brush("#222327");
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto,Auto,Auto"), Margin = new Thickness(14), RowSpacing = 8 };
        var setup = new StackPanel { Spacing = 6 };
        setup.Children.Add(new TextBlock { Text = "Build with Codex", FontSize = 20 });
        setup.Children.Add(new TextBlock { Text = "Uses your installed Codex login. Changes reach the same world as the editor.", TextWrapping = TextWrapping.Wrap });
        setup.Children.Add(executable); setup.Children.Add(engine); setup.Children.Add(model); setup.Children.Add(thread); setup.Children.Add(allowEdits); setup.Children.Add(connect);
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
        AutomationProperties.SetName(allowEdits, "Agent allow world edits");
        connect.Click += async (_, _) => await Connect();
        send.Click += async (_, _) => await Send();
        stop.Click += async (_, _) => await Stop();
        Closing += (_, _) => { closing = true; lifetime.Cancel(); };
        Closed += async (_, _) => { if (session is { } current) await DisposeSession(current); };
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
        var live = ready && session?.ThreadId is not null && !closing;
        send.IsEnabled = live && activeTurn is null && !submitting;
        stop.IsEnabled = live && activeTurn is not null;
        connect.IsEnabled = !connecting && session is null && !closing;
        executable.IsEnabled = engine.IsEnabled = model.IsEnabled = connect.IsEnabled;
        thread.IsReadOnly = !connect.IsEnabled; thread.IsEnabled = !closing;
        allowEdits.IsEnabled = connect.IsEnabled;
    }
    private async Task Connect()
    {
        connecting = true; Controls();
        status.Text = "Connecting…";
        CodexSession? current = null;
        using var startup = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        startup.CancelAfter(TimeSpan.FromSeconds(60));
        try
        {
            current = await CodexSession.ConnectAsync(executable.Text ?? "", engine.Text ?? "", endpoint, projectRoot, startup.Token, allowEdits.IsChecked == true);
            session = current;
            if (closing) { await current.DisposeAsync(); return; }
            _ = ReadEvents(current);
            var catalog = await current.ListModelsAsync(startup.Token);
            var available = (catalog?["data"] as JsonArray ?? throw new IOException("Codex returned no model catalog."))
                .OfType<JsonObject>().Where(item => item["hidden"]?.GetValue<bool>() != true).ToArray();
            var chosen = string.IsNullOrWhiteSpace(model.Text)
                ? available.FirstOrDefault(item => item["isDefault"]?.GetValue<bool>() == true)?["model"]?.GetValue<string>()
                : model.Text.Trim();
            if (string.IsNullOrWhiteSpace(chosen) || !available.Any(item => item["model"]?.GetValue<string>() == chosen))
                throw new InvalidOperationException("Choose a model offered by this Codex CLI: " + string.Join(", ", available.Select(item => item["model"]?.GetValue<string>())));
            model.Text = chosen;
            ToolTip.SetTip(model, "Models offered by this CLI: " + string.Join(", ", available.Select(item => item["model"]?.GetValue<string>())));
            var id = await current.OpenAsync(string.IsNullOrWhiteSpace(thread.Text) ? null : thread.Text.Trim(), startup.Token, chosen);
            if (!ReferenceEquals(session, current)) throw new IOException("Connection ended while opening the conversation.");
            thread.Text = id; ready = true;
            status.Text = "Connected · copy the conversation ID above to reopen this conversation later.";
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
    private async Task Send()
    {
        if (!ready || session is not { } current || activeTurn is not null || submitting || string.IsNullOrWhiteSpace(prompt.Text)) return;
        completed.Clear();
        var text = prompt.Text; submitting = true; Controls();
        Append("\nYou: " + text + "\n\nCodex: "); prompt.Text = ""; status.Text = "Working…";
        try
        {
            var id = await current.SendAsync(text, lifetime.Token);
            if (ReferenceEquals(session, current) && !completed.Contains(id)) activeTurn = id;
        }
        catch (Exception error)
        {
            if (ReferenceEquals(session, current))
            {
                status.Text = error.Message;
                if (error is AgentExchangeException { OutcomeUnknown: true }) LoseConnection(current, error.Message);
            }
        }
        finally { if (ReferenceEquals(session, current)) submitting = false; Controls(); }
    }
    private async Task Stop()
    {
        if (session is not { } current || activeTurn is not { } id) return;
        stop.IsEnabled = false;
        try
        {
            await current.InterruptAsync(id, lifetime.Token);
            if (ReferenceEquals(session, current) && activeTurn == id) status.Text = "Stop requested; waiting for the turn to finish. Existing edits remain.";
        }
        catch (Exception error) { status.Text = error.Message; Controls(); }
    }
    private async Task ReadEvents(CodexSession current)
    {
        try
        {
            await foreach (var message in current.Events.ReadAllAsync(lifetime.Token).ConfigureAwait(false))
                await Dispatcher.UIThread.InvokeAsync(() => Handle(current, message));
        }
        catch (Exception error)
        {
            await Dispatcher.UIThread.InvokeAsync(() =>
            {
                if (!closing && ReferenceEquals(session, current))
                    LoseConnection(current, "Connection ended: " + error.Message);
            });
        }
    }
    private void LoseConnection(CodexSession current, string message)
    {
        if (!ReferenceEquals(session, current)) return;
        session = null; ready = false; activeTurn = null; submitting = false;
        approvals.Children.Clear(); status.Text = message; Controls();
        _ = DisposeSession(current);
    }
    private async Task DisposeSession(CodexSession current)
    {
        try { await current.DisposeAsync(); }
        catch (Exception error) { if (!closing) status.Text = "Agent shutdown failed: " + error.Message; }
    }
    private void Handle(CodexSession current, JsonObject message)
    {
        if (closing || !ReferenceEquals(session, current)) return;
        var method = message["method"]?.GetValue<string>() ?? "";
        var parameters = message["params"] as JsonObject;
        if (message["id"] is JsonNode requestId)
        {
            if (method is "item/commandExecution/requestApproval" or "item/fileChange/requestApproval" && approvals.Children.Count < 16)
            {
                var card = new StackPanel { Spacing = 4 };
                card.Children.Add(new TextBlock { Text = method + "\n" + parameters?.ToJsonString(), TextWrapping = TextWrapping.Wrap });
                var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                foreach (var decision in new[] { "accept", "decline" })
                {
                    var button = new Button { Content = decision == "accept" ? "Approve once" : "Decline" };
                    button.Click += async (_, _) =>
                    {
                        card.IsEnabled = false;
                        try { await current.ReplyAsync(requestId, new JsonObject { ["decision"] = decision }, lifetime.Token); approvals.Children.Remove(card); }
                        catch (Exception error) { status.Text = error.Message; }
                    };
                    buttons.Children.Add(button);
                }
                card.Children.Add(buttons); approvals.Children.Add(card);
            }
            else _ = RejectUnsupported(current, requestId, method);
            return;
        }
        if (parameters?["threadId"]?.GetValue<string>() != current.ThreadId) return;
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
    private async Task RejectUnsupported(CodexSession current, JsonNode id, string method)
    {
        try { await current.RejectAsync(id, "This Poima client does not yet support " + method, lifetime.Token); status.Text = "Unsupported request declined: " + method; }
        catch (Exception error) { if (!closing) status.Text = error.Message; }
    }
    internal JsonObject InspectForQualification() => new()
    {
        ["ready"] = ready, ["connecting"] = connecting, ["send_enabled"] = send.IsEnabled,
        ["stop_enabled"] = stop.IsEnabled, ["connect_enabled"] = connect.IsEnabled,
        ["thread_id"] = session?.ThreadId, ["turn_id"] = activeTurn,
        ["status"] = status.Text, ["transcript"] = transcript.Text,
        ["approvals"] = approvals.Children.Count
    };
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
