// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json;
using Avalonia.Controls;
using Poima.AgentHost;

namespace Poima.Editor;

internal sealed class CreatorBuildTeam
{
    private const int MaximumWorkers = 10;
    private readonly ProjectLocation project;
    private readonly TabControl conversations;
    private readonly Func<string, bool> addToDraft;
    private readonly Action<string?> status;
    private readonly List<WorkerDraft> workers = [];
    private string? groupDirectory;
    private (string Checkpoint, string Hash, long Revision, string Source)? snapshot;
    private Task? provision, retirement;
    private bool closing, adding;
    public bool CanAdd => !closing && !adding && workers.Count < MaximumWorkers;
    public bool CanReview => !closing && !adding && workers.Any(worker => !worker.Pane.Busy && worker.Pane.Finding is not null);
    public long BaseRevision => snapshot?.Revision ?? throw new InvalidOperationException("There is no worker checkpoint.");

    private sealed record WorkerDraft(CreatorReviews.WorkerPane Pane, string Manifest);
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
            if (groupDirectory is null)
            {
                var parent = Path.GetDirectoryName(project.Root) ??
                    throw new InvalidOperationException("The project needs a parent folder for worker drafts.");
                var drafts = Directory.CreateDirectory(Path.Combine(parent, ".poima-drafts"));
                if ((drafts.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidOperationException("Choose an ordinary folder for worker drafts.");
                groupDirectory = Directory.CreateDirectory(Path.Combine(drafts.FullName, Guid.NewGuid().ToString("N")[..12])).FullName;
            }
            if (snapshot is null)
            {
                var checkpoint = await ProjectFiles.Command("checkpoint", project.Manifest,
                    "--output", Path.Combine(groupDirectory, "base"));
                snapshot = (checkpoint.GetProperty("checkpoint").GetString()!,
                    checkpoint.GetProperty("source_sha256").GetString()!, checkpoint.GetProperty("revision").GetInt64(),
                    checkpoint.GetProperty("source_manifest").GetString()!);
            }
            if (closing) return;
            var captured = snapshot.Value;
            var number = workers.Count + 1;
            var draft = await ProjectFiles.Command("draft", captured.Checkpoint,
                "--output", Path.Combine(groupDirectory, "w" + number), "--expected-base-sha256", captured.Hash);
            if (closing) return;
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
            workers.Add(new(pane, manifest));
            var tab = CreatorWindow.ConversationTab(name, pane.View);
            conversations.Items.Add(tab);
            conversations.SelectedItem = tab;
            status(null);
        }
        catch (Exception failure) { if (!closing) status(failure.Message); }
        finally { adding = false; }
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
