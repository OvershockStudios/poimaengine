// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia.Controls;
using Poima.AgentHost;

namespace Poima.Editor;

internal sealed class CreatorBuildTeam
{
    private const int MaximumAssignments = 64;
    private readonly ProjectLocation project;
    private readonly TabControl conversations;
    private readonly Func<string, bool> addToDraft;
    private readonly Action<string?> status;
    private readonly List<WorkerDraft> workers = [];
    private readonly Dictionary<string, LeadBatch> batches = new(StringComparer.Ordinal);
    private string? groupDirectory;
    private (string Checkpoint, string Hash, long Revision, string Source)? snapshot;
    private Task? provision, retirement;
    private bool closing, adding;
    public bool CanAdd => !closing && !adding && workers.Count < CodexWorkerTool.MaximumWorkers;
    public bool CanReview => !closing && !adding && workers.Any(worker => !worker.Pane.Busy && worker.Pane.Finding is not null);
    public long BaseRevision => snapshot?.Revision ?? throw new InvalidOperationException("There is no worker checkpoint.");

    private sealed record WorkerDraft(CreatorReviews.WorkerPane Pane, string Manifest);
    private sealed record Assignment(WorkerDraft Worker, Task<CreatorReviews.WorkerPane.TurnResult> Turn);

    private sealed class LeadBatch(CodexWorkerRequest request)
    {
        public CodexWorkerRequest Request { get; } = request;
        public Assignment[] Assignments { get; set; } = [];
        public string? Error { get; set; }
        public TaskCompletionSource Changed { get; private set; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool Pending => Error is null && (Assignments.Length == 0 || Assignments.Any(work => !work.Turn.IsCompleted));

        public void SignalChange()
        {
            var previous = Changed;
            Changed = new(TaskCreationOptions.RunContinuationsAsynchronously);
            previous.TrySetResult();
        }
    }
    public sealed record ReviewedDraft(string Name, string Manifest, long Revision, string WorldHash, string Report);
    public sealed record CombinedCheck(string ProjectId, string History, string Checkpoint, string Snapshot,
        string GameManifest, PlaytestAttempt Attempt);

    public CreatorBuildTeam(ProjectLocation project, TabControl conversations, Func<string, bool> addToDraft,
        Action<string?> status)
    {
        this.project = project;
        this.conversations = conversations;
        this.addToDraft = addToDraft;
        this.status = status;
    }

    public Task AddAsync(string executable, string? model)
    {
        if (!CanAdd) return Task.CompletedTask;
        return provision = AddCoreAsync(executable, model);
    }

    private async Task AddCoreAsync(string executable, string? model)
    {
        adding = true;
        status("Preparing a worker draft…");
        try
        {
            await CaptureAsync(null);
            if (closing) return;
            var worker = await DraftAsync(workers.Count + 1, executable, model);
            if (closing) return;
            conversations.SelectedItem = AddTab(worker);
            status(null);
        }
        catch (Exception failure) { if (!closing) status(failure.Message); }
        finally { adding = false; }
    }

    private async Task CaptureAsync(long? expectedRevision)
    {
        if (snapshot is { } existing)
        {
            if (expectedRevision is not null && expectedRevision != existing.Revision)
                throw new InvalidOperationException($"This worker round uses revision {existing.Revision}. Reopen the project to start a round from the current scene.");
            return;
        }
        if (expectedRevision is long expected)
        {
            var current = await ProjectFiles.Command("inspect", project.Manifest);
            if (current.GetProperty("project_id").GetString() != project.ProjectId ||
                current.GetProperty("revision").GetInt64() != expected)
                throw new InvalidOperationException("The project or scene revision changed. Inspect the current project before assigning work.");
        }
        if (closing) return;
        if (groupDirectory is null)
        {
            var parent = Path.GetDirectoryName(project.Root) ??
                throw new InvalidOperationException("The project needs a parent folder for worker drafts.");
            var drafts = Directory.CreateDirectory(Path.Combine(parent, ".poima-drafts"));
            if ((drafts.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidOperationException("Choose an ordinary folder for worker drafts.");
            groupDirectory = Directory.CreateDirectory(Path.Combine(drafts.FullName, Guid.NewGuid().ToString("N")[..12])).FullName;
        }
        var checkpoint = await ProjectFiles.Command("checkpoint", project.Manifest,
            "--output", Path.Combine(groupDirectory, "base"));
        if (closing) return;
        var revision = checkpoint.GetProperty("revision").GetInt64();
        var source = checkpoint.GetProperty("source_manifest").GetString()!;
        var captured = await ProjectFiles.Command("inspect", source);
        if (captured.GetProperty("project_id").GetString() != project.ProjectId ||
            expectedRevision is not null && expectedRevision != revision)
        {
            groupDirectory = null;
            throw new InvalidOperationException("The project changed while capturing the worker checkpoint. Check the current project and revision.");
        }
        snapshot = (checkpoint.GetProperty("checkpoint").GetString()!,
            checkpoint.GetProperty("source_sha256").GetString()!, revision, source);
    }

    private async Task<WorkerDraft> DraftAsync(int number, string executable, string? model)
    {
        var captured = snapshot!.Value;
        var suffix = Guid.NewGuid().ToString("N")[..8];
        var draft = await ProjectFiles.Command("draft", captured.Checkpoint,
            "--output", Path.Combine(groupDirectory!, $"w{number}-{suffix}"),
            "--expected-base-sha256", captured.Hash);
        var manifest = draft.GetProperty("manifest").GetString()!;
        var projectId = draft.GetProperty("project_id").GetString()!;
        var name = "Worker " + number;
        var engine = Path.Combine(AppContext.BaseDirectory, "poima.exe");
        var scope = $"You are editing Poima draft {projectId}, forked from authored revision {captured.Revision}. " +
            "Make the assigned change using the draft Poima tools. Verify the resulting draft revision and entities. " +
            "Report the changed entity IDs, verification results, and any separate source files the lead should select. " +
            "The lead reviews the pinned draft scene and verifies the assembled candidate.";
        var pane = new CreatorReviews.WorkerPane(name, "Assign part of the game to this worker.",
            cancellation => CodexReview.OpenDraftAsync(executable, engine, manifest, model, cancellation),
            () => scope, draftPath: manifest, useReport: report =>
            {
                if (closing) return;
                var message = $"{name} completed a task on an independent Poima draft.\n" +
                    $"The base is `{captured.Checkpoint}` at authored revision {captured.Revision}.\n" +
                    $"Its SHA-256 is `{captured.Hash}`. The worker manifest is `{manifest}`.\n\n" +
                    "Review the draft, then use project inspect to get its world_sha256 for project integrate. " +
                    "Poima combines pinned scene edits and copies their cooked dependencies. Select other source files by hash. " +
                    "Build and verify the resulting candidate before offering it for play.\n\n" + report;
                status(addToDraft(message) ? "Worker report added to the main chat draft."
                    : "Wait for the main chat to finish before adding the report.");
            });
        return new(pane, manifest);
    }

    private TabItem AddTab(WorkerDraft worker)
    {
        workers.Add(worker);
        var tab = CreatorWindow.ConversationTab(worker.Pane.Name, worker.Pane.View);
        conversations.Items.Add(tab);
        return tab;
    }

    public async Task<JsonObject> CoordinateAsync(CodexWorkerRequest request, string executable, string? model,
        CancellationToken cancellation)
    {
        if (batches.TryGetValue(request.RequestId, out var existing))
        {
            if (request.ExpectedRevision is not null &&
                (existing.Request.ExpectedRevision != request.ExpectedRevision || !existing.Request.Tasks.SequenceEqual(request.Tasks)))
                throw new InvalidOperationException("This request_id already identifies a different assignment.");
            if (request.WaitMilliseconds > 0 && existing.Pending)
            {
                try { await existing.Changed.Task.WaitAsync(TimeSpan.FromMilliseconds(request.WaitMilliseconds), cancellation); }
                catch (TimeoutException) { }
            }
            return Describe(existing, replayed: request.ExpectedRevision is not null);
        }
        if (request.ExpectedRevision is null) throw new InvalidOperationException("Unknown worker assignment. Inspect a request_id returned by assign.");
        if (!CanAdd || workers.Count + request.Tasks.Length > CodexWorkerTool.MaximumWorkers)
            throw new InvalidOperationException("Wait for draft preparation or assign within the workspace limit of ten workers.");
        if (batches.Count >= MaximumAssignments) throw new InvalidOperationException("Reopen the project to start a new worker round.");
        var batch = new LeadBatch(request);
        batches.Add(request.RequestId, batch);
        provision = StartBatchAsync(batch, executable, model);
        return Describe(batch, replayed: false);
    }

    private async Task StartBatchAsync(LeadBatch batch, string executable, string? model)
    {
        adding = true;
        status("Preparing worker drafts…");
        try
        {
            await CaptureAsync(batch.Request.ExpectedRevision);
            if (closing) return;
            var staged = new List<WorkerDraft>();
            foreach (var _ in batch.Request.Tasks)
            {
                staged.Add(await DraftAsync(workers.Count + staged.Count + 1, executable, model));
                if (closing) return;
            }
            foreach (var worker in staged) AddTab(worker);
            batch.Assignments = staged.Select((worker, index) =>
                new Assignment(worker, FinishAssignmentAsync(batch, worker, batch.Request.Tasks[index]))).ToArray();
            status(null);
        }
        catch (Exception failure)
        {
            batch.Error = failure.Message;
            if (!closing) status(failure.Message);
        }
        finally
        {
            adding = false;
            if (closing && batch.Assignments.Length == 0) batch.Error ??= "The workspace closed during draft preparation.";
            batch.SignalChange();
        }
    }

    private static async Task<CreatorReviews.WorkerPane.TurnResult> FinishAssignmentAsync(
        LeadBatch batch, WorkerDraft worker, string task)
    {
        try { return await worker.Pane.RunAsync(task); }
        catch (Exception failure) { return new("failed", null, failure.Message); }
        finally { batch.SignalChange(); }
    }

    private static JsonObject Describe(LeadBatch batch, bool replayed)
    {
        var rows = new JsonArray();
        var allCompleted = true;
        foreach (var assignment in batch.Assignments)
        {
            var turn = assignment.Turn.IsCompletedSuccessfully ? assignment.Turn.Result : null;
            var row = new JsonObject { ["worker"] = assignment.Worker.Pane.Name, ["manifest"] = assignment.Worker.Manifest,
                ["state"] = turn?.State ?? "running" };
            allCompleted &= turn?.State == "completed";
            if (turn?.Report is { } report)
            {
                row["report"] = report.Length <= 4000 ? report : report[..4000];
                if (report.Length > 4000) row["report_truncated"] = true;
            }
            if (turn?.Error is { } error) row["error"] = error;
            rows.Add(row);
        }
        var state = batch.Error is not null ? "failed" : rows.Count == 0 ? "preparing" :
            batch.Pending ? "running" : allCompleted ? "completed" : "failed";
        var result = new JsonObject { ["request_id"] = batch.Request.RequestId, ["base_revision"] = batch.Request.ExpectedRevision,
            ["state"] = state, ["replayed"] = replayed, ["workers"] = rows };
        if (batch.Error is not null) result["error"] = batch.Error;
        return result;
    }

    public async Task<ReviewedDraft[]> ReviewAsync(CancellationToken cancellation)
    {
        var baseSource = snapshot?.Source ?? throw new InvalidOperationException("There is no worker checkpoint.");
        if (!File.Exists(Path.Combine(Path.GetDirectoryName(baseSource)!, ProjectFiles.VerificationSuiteName)))
            throw new InvalidOperationException("This worker round started without a playtest suite. Add game.verify.json, then reopen the project and start a new round.");

        var result = new List<ReviewedDraft>();
        foreach (var worker in workers)
        {
            if (worker.Pane.Busy || worker.Pane.Finding is not { } report) continue;
            var draft = await ProjectFiles.Command(cancellation, "inspect", worker.Manifest);
            if (worker.Pane.Busy || worker.Pane.Finding != report) continue;
            result.Add(new(worker.Pane.Name, worker.Manifest, draft.GetProperty("revision").GetInt64(),
                draft.GetProperty("world_sha256").GetString()!, report));
        }
        return [.. result];
    }

    public async Task<CombinedCheck> CombineAsync(ReviewedDraft[] selected, Action<string> setProgress,
        CancellationToken cancellation)
    {
        var captured = snapshot ?? throw new InvalidOperationException("There is no worker checkpoint.");
        if (selected.Length == 0 || !selected.All(draft => workers.Any(worker =>
                worker.Manifest == draft.Manifest && !worker.Pane.Busy && worker.Pane.Finding == draft.Report)))
            throw new InvalidOperationException("A selected worker has changed its report. Review the drafts again.");

        var root = Directory.CreateDirectory(Path.Combine(groupDirectory!, "combined-" + Guid.NewGuid().ToString("N")[..12])).FullName;
        var request = Path.Combine(root, "selection.json");
        await File.WriteAllTextAsync(request, JsonSerializer.Serialize(new
        {
            base_sha256 = captured.Hash,
            drafts = selected.Select(draft => new { manifest = draft.Manifest, world_sha256 = draft.WorldHash, files = Array.Empty<string>() })
        }), cancellation);
        var merged = await ProjectFiles.Command("integrate", captured.Checkpoint,
            "--output", Path.Combine(root, "project"), "--request", request);
        cancellation.ThrowIfCancellationRequested();
        var manifest = merged.GetProperty("manifest").GetString()!;
        var projectId = merged.GetProperty("project_id").GetString()!;
        var history = Directory.CreateDirectory(Path.Combine(root, ".poima-history", projectId)).FullName;
        var time = DateTime.UtcNow.ToString("yyyyMMdd-HHmmssfff", CultureInfo.InvariantCulture);
        var folder = Path.Combine(history, time + "-" + Guid.NewGuid().ToString("N")[..8]);
        var capturedMerged = await ProjectFiles.Command("checkpoint", manifest, "--output", folder);
        var source = capturedMerged.GetProperty("source_manifest").GetString()!;
        var sourceInfo = await ProjectFiles.Command(cancellation, "inspect", source);
        if (sourceInfo.GetProperty("world_sha256").GetString() != merged.GetProperty("world_sha256").GetString())
            throw new InvalidOperationException("The combined scene changed before it was captured.");

        setProgress("Building the combined game…");
        var build = await ProjectFiles.Command("build", source,
            "--output", Path.Combine(folder, "game"), "--runtime", ProjectFiles.PreviewRuntime);
        cancellation.ThrowIfCancellationRequested();
        var suite = Path.Combine(Path.GetDirectoryName(source)!, ProjectFiles.VerificationSuiteName);
        var report = Path.Combine(folder, "checks.json");
        setProgress("Checking the combined game…");
        var verification = await ProjectFiles.VerifyExport(build.GetProperty("output").GetString()!, suite, report, cancellation);
        var versions = await ProjectFiles.Command(cancellation, "versions", history, "--project-id", projectId);
        var entry = versions.GetProperty("entries").EnumerateArray().SingleOrDefault();
        if (entry.ValueKind != JsonValueKind.Object ||
            !string.Equals(Path.GetFullPath(entry.GetProperty("report").GetString()!), report, StringComparison.OrdinalIgnoreCase) ||
            entry.GetProperty("bundle_sha256").GetString() != verification.GetProperty("bundle_sha256").GetString() ||
            entry.GetProperty("passed").GetBoolean() != verification.GetProperty("passed").GetBoolean())
            throw new InvalidOperationException("The combined game's results could not be paired with its source checkpoint.");

        return new(projectId, history, entry.GetProperty("checkpoint").GetString()!,
            entry.GetProperty("snapshot_sha256").GetString()!, entry.GetProperty("game_manifest").GetString()!,
            PlaytestAttempt.FromHistory(entry));
    }

    public Task RetireAsync() => retirement ??= RetireCoreAsync();

    private async Task RetireCoreAsync()
    {
        closing = true;
        foreach (var worker in workers) worker.Pane.Enable(false);
        if (provision is { } pending) await pending;
        await Task.WhenAll(workers.Select(worker => worker.Pane.ResetAsync()));
    }
}
