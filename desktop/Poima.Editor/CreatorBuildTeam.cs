// SPDX-License-Identifier: Apache-2.0
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
    private readonly List<CreatorReviews.WorkerPane> workers = [];
    private string? groupDirectory;
    private (string Checkpoint, string Hash, long Revision)? snapshot;
    private Task? provision, retirement;
    private bool closing, adding;
    public bool CanAdd => !closing && !adding && workers.Count < MaximumWorkers;

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
                    checkpoint.GetProperty("source_sha256").GetString()!, checkpoint.GetProperty("revision").GetInt64());
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
                "Tell the lead which authored operations and asset files are needed for integration. Include entity IDs, " +
                "exact settings and asset hashes where relevant. The lead reviews and verifies the joined candidate.";
            var pane = new CreatorReviews.WorkerPane(name, "Assign part of the game to this worker.",
                cancellation => CodexReview.OpenDraftAsync(executable, engine, manifest, model, cancellation),
                () => scope, draftPath: manifest, useReport: report =>
                {
                    if (closing) return;
                    var message = $"{name} completed a task on an independent Poima draft.\n" +
                        $"The base is `{captured.Checkpoint}` at authored revision {captured.Revision}.\n" +
                        $"Its SHA-256 is `{captured.Hash}`. The worker manifest is `{manifest}`.\n\n" +
                        "Inspect the proposed changes before staging them with project integrate. " +
                        "Build and verify the resulting candidate before offering it for play.\n\n" + report;
                    status(addToDraft(message) ? "Worker report added to the main chat draft."
                        : "Wait for the main chat to finish before adding the report.");
                });
            workers.Add(pane);
            var tab = CreatorWindow.ConversationTab(name, pane.View);
            conversations.Items.Add(tab);
            conversations.SelectedItem = tab;
            status(null);
        }
        catch (Exception failure) { if (!closing) status(failure.Message); }
        finally { adding = false; }
    }

    public Task RetireAsync() => retirement ??= RetireCoreAsync();

    private async Task RetireCoreAsync()
    {
        closing = true;
        foreach (var worker in workers) worker.Enable(false);
        if (provision is { } pending) await pending;
        await Task.WhenAll(workers.Select(worker => worker.ResetAsync()));
    }
}
