// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Media;
using Poima.AgentHost;

namespace Poima.Editor;

internal sealed class CreatorReviews
{
    private sealed record Assignment(string World, long Revision);
    private readonly NativeHost host;
    private readonly Func<string, bool> addToDraft;
    private readonly WorkerPane[] panes;
    private readonly TextBlock scopeNote = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button refresh = new() { Content = "Review current revision" };
    private readonly Button use = new() { Content = "Use findings in chat" };
    private Assignment assignment;
    private bool closing, refreshing;
    private Task? retirement, assignmentRefresh;
    public TabItem[] Tabs { get; }
    public Control Footer { get; }

    public CreatorReviews(NativeHost host, string directory, string executable, string? model,
        Func<string, bool> addToDraft)
    {
        this.host = host;
        this.addToDraft = addToDraft;
        assignment = ReadAssignment();
        use.Background = EditorTheme.Brush("#722F37");
        use.Foreground = EditorTheme.Brush("#F7E8EB");
        AutomationProperties.SetAutomationId(scopeNote, "specialist-scope");

        panes = [
            new("Specialist 1", "What should this reviewer check?", OpenReview, EnsureCurrent, UpdateControls),
            new("Specialist 2", "Give this reviewer a different part of the scene.", OpenReview, EnsureCurrent, UpdateControls),
        ];
        Tabs = panes.Select(pane => CreatorWindow.ConversationTab(pane.Name, pane.View)).ToArray();
        var actions = new WrapPanel { ItemSpacing = 8, LineSpacing = 6, Children = { use, refresh } };
        Footer = new StackPanel { Spacing = 8, Margin = new Thickness(14, 8, 14, 14), Children = { scopeNote, actions } };

        use.Click += (_, _) => UseFindings();
        refresh.Click += async (_, _) => await (assignmentRefresh = RefreshAssignment());
        ShowAssignment();
        UpdateControls();

        Task<CodexReview> OpenReview(CancellationToken cancellation) => CodexReview.OpenAsync(
            executable, Path.Combine(AppContext.BaseDirectory, "poima.exe"), host.Endpoint, directory,
            assignment.Revision, model, cancellation);
    }

    private Assignment ReadAssignment()
    {
        var world = host.Call("world.inspect");
        return new(world["world_id"]!.GetValue<string>(), world["revision"]!.GetValue<long>());
    }

    private string EnsureCurrent()
    {
        if (ReadAssignment() != assignment)
            throw new InvalidOperationException("The scene changed. Review the current revision to continue.");
        return $"You are a Poima specialist inspecting world {assignment.World} at authored revision {assignment.Revision}. " +
            "Confirm the world and revision with poima_inspect and world.inspect before reading relevant scene data. " +
            "Use the scoped Poima tools. Report findings with entity IDs and measurements where relevant. The lead decides and verifies edits.";
    }

    private void ShowAssignment() => scopeNote.Text =
        $"Reviewing revision {assignment.Revision}. Each specialist can inspect the scene and propose changes.";

    private void UpdateControls()
    {
        var busy = panes.Any(pane => pane.Busy);
        refresh.IsEnabled = !closing && !refreshing && !busy;
        use.IsEnabled = refresh.IsEnabled && panes.Any(pane => pane.Finding is not null);
    }

    private async Task RefreshAssignment()
    {
        if (!refresh.IsEnabled) return;
        refreshing = true;
        UpdateControls();
        foreach (var pane in panes) pane.Enable(false);
        try
        {
            await Task.WhenAll(panes.Select(pane => pane.ResetAsync()));
            if (closing) return;
            assignment = ReadAssignment();
            ShowAssignment();
        }
        catch (Exception error) { scopeNote.Text = error.Message; }
        finally
        {
            refreshing = false;
            if (!closing) foreach (var pane in panes) pane.Enable(true);
            UpdateControls();
        }
    }

    private void UseFindings()
    {
        if (!use.IsEnabled) return;
        try
        {
            EnsureCurrent();
            var findings = panes.Where(pane => pane.Finding is not null)
                .Select(pane => pane.Name + "\n" + pane.Finding);
            var proposed = $"Specialist findings for world {assignment.World}, revision {assignment.Revision}. " +
                "Inspect the current scene and verify these proposals before editing.\n\n" + string.Join("\n\n", findings);
            scopeNote.Text = addToDraft(proposed)
                ? "Findings added to the main chat draft."
                : "Wait for the main chat to finish before adding findings.";
        }
        catch (Exception error) { scopeNote.Text = error.Message; }
    }

    public Task RetireAsync() => retirement ??= RetireCoreAsync();

    private async Task RetireCoreAsync()
    {
        closing = true;
        UpdateControls();
        scopeNote.Text = "Closing specialist sessions…";
        foreach (var pane in panes) pane.Enable(false);
        if (assignmentRefresh is { } work) await work;
        await Task.WhenAll(panes.Select(pane => pane.ResetAsync()));
    }

    internal sealed class WorkerPane
    {
        private readonly Func<CancellationToken, Task<CodexReview>> open;
        private readonly Func<string> scope;
        private readonly Action? changed;
        private readonly TextBox task = new() { AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, Height = 85, MaxLength = 4000 };
        private readonly TextBox transcript = new() { AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, IsReadOnly = true };
        private readonly TextBlock status = new() { Text = "Ready", TextWrapping = TextWrapping.Wrap };
        private readonly Button ask = new();
        private readonly Button stop = new() { IsEnabled = false };
        private readonly bool editableDraft;
        private readonly Button? handoff;
        private CodexReview? review;
        private CancellationTokenSource? active;
        private Task? pending;
        private bool enabled = true;
        public string Name { get; }
        public Control View { get; }
        public string? Finding { get; private set; }
        public bool Busy => active is not null;

        public WorkerPane(string name, string placeholder, Func<CancellationToken, Task<CodexReview>> open,
            Func<string> scope, Action? changed = null, string? draftPath = null, Action<string>? useReport = null)
        {
            Name = name;
            this.open = open; this.scope = scope; this.changed = changed;
            editableDraft = draftPath is not null;
            task.PlaceholderText = placeholder;
            ask.Content = editableDraft ? "Send to worker" : "Send to specialist";
            stop.Content = editableDraft ? "Stop worker" : "Stop review";
            AutomationProperties.SetName(task, editableDraft ? "Worker task" : "Specialist task");
            AutomationProperties.SetName(transcript, editableDraft ? "Worker report" : "Specialist findings");
            AutomationProperties.SetAutomationId(status, editableDraft ? "worker-status" : "specialist-status");
            transcript.Classes.Add("conversation");
            ask.Classes.Add("primary");
            var buttons = new WrapPanel { ItemSpacing = 8, Children = { ask, stop } };
            if (useReport is not null)
            {
                handoff = new Button { Content = "Add report to main chat", IsEnabled = false };
                handoff.Click += (_, _) => { if (Finding is { } report) useReport(report); };
                buttons.Children.Add(handoff);
            }
            var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto,Auto"), RowSpacing = 8, Margin = new Thickness(14) };
            root.Children.Add(status);
            Grid.SetRow(transcript, 1); root.Children.Add(transcript);
            Grid.SetRow(task, 2); root.Children.Add(task);
            Grid.SetRow(buttons, 3); root.Children.Add(buttons);
            View = root;
            ask.Click += async (_, _) =>
            {
                if (ask.IsEnabled && task.Text is { } text) await (pending = SendAsync(text));
            };
            stop.Click += (_, _) =>
            {
                if (active is null) return;
                status.Text = editableDraft ? "Stopping worker…" : "Stopping review…";
                active.Cancel();
                Update();
            };
            task.TextChanged += (_, _) => Update();
            ask.IsEnabled = false;
            if (draftPath is not null) ToolTip.SetTip(status, draftPath);
        }

        public void Enable(bool value) { enabled = value; Update(); }

        private void Update()
        {
            task.IsReadOnly = Busy || !enabled;
            ask.IsEnabled = enabled && !Busy && !string.IsNullOrWhiteSpace(task.Text);
            stop.IsEnabled = enabled && active is { IsCancellationRequested: false };
            if (handoff is not null) handoff.IsEnabled = enabled && !Busy && Finding is not null;
            changed?.Invoke();
        }

        private async Task SendAsync(string text)
        {
            using var cancellation = new CancellationTokenSource();
            active = cancellation;
            Finding = null;
            status.Text = editableDraft ? "Working in draft…" : "Reviewing…";
            Update();
            try
            {
                var context = scope();
                if (review is null)
                {
                    using var startup = CancellationTokenSource.CreateLinkedTokenSource(cancellation.Token);
                    startup.CancelAfter(TimeSpan.FromSeconds(60));
                    review = await open(startup.Token);
                }
                transcript.Text = "";
                var answer = await review.AskAsync(context + "\n\n" + text.Trim(),
                    draft => transcript.Text = draft, cancellation.Token);
                cancellation.Token.ThrowIfCancellationRequested();
                _ = scope();
                Finding = transcript.Text = answer;
                task.Text = "";
                status.Text = editableDraft ? "Worker finished" : "Review complete";
            }
            catch (Exception error)
            {
                try
                {
                    await DisposeReviewer();
                    status.Text = cancellation.IsCancellationRequested
                        ? editableDraft ? "Worker stopped" : "Review stopped"
                        : error.Message;
                }
                catch (Exception shutdown) { status.Text = "Couldn’t close the specialist connection. " + shutdown.Message; }
            }
            finally { active = null; Update(); }
        }

        public async Task ResetAsync()
        {
            active?.Cancel();
            if (pending is { } job) await job;
            await DisposeReviewer();
            Finding = null;
            transcript.Text = "";
            status.Text = "Ready";
        }

        private async Task DisposeReviewer()
        {
            var current = review;
            review = null;
            if (current is not null) await current.DisposeAsync();
        }
    }
}
