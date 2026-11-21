// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia.Controls;
using Poima.AgentHost;
using static Poima.Editor.CreatorWorkerRoundStore;

namespace Poima.Editor;

internal sealed class CreatorBuildTeam
{
    private readonly ProjectLocation project;
    private readonly TabControl conversations;
    private readonly Func<string, bool> addToDraft;
    private readonly Action<string?> status;
    private readonly Action<CombinedCheck> checkedBuild;
    private readonly Func<(string Executable, string? Model)?> client;
    private readonly CreatorWorkerRoundStore store;
    private readonly List<WorkerTab> tabs = [];
    private readonly List<Task> managedTurns = [];
    private Round? round;
    private Task? provision, verificationWork, retirement;
    private CancellationTokenSource? verifying;
    private TaskCompletionSource changed = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private bool closing, adding, loaded, restoreFailed;

    public bool HasWorkers => tabs.Count != 0;
    public bool IsVerifying => verifying is not null;
    public CombinedCheck? Proposal { get; private set; }
    public bool CanAdd => loaded && !restoreFailed && !closing && !adding && !IsVerifying && tabs.Count < CodexWorkerTool.MaximumWorkers;
    public bool CanReview => !closing && !adding && !IsVerifying && tabs.Any(tab => !tab.Pane.Busy && tab.Pane.Finding is not null);
    public bool CanStartRound => loaded && round is not null && !adding && !closing && !IsVerifying &&
        tabs.All(tab => !tab.Pane.Busy) && managedTurns.All(task => task.IsCompleted);
    public long BaseRevision => round?.Base?.Revision ?? throw new InvalidOperationException("There is no worker checkpoint.");

    private sealed record WorkerTab(CreatorReviews.WorkerPane Pane, TabItem Tab, bool Available);
    public sealed record ReviewedDraft(string Name, string Manifest, long Revision, string WorldHash, string Report);
    public sealed record CombinedCheck(string ProjectId, string History, string Checkpoint, string Snapshot,
        string GameManifest, PlaytestAttempt Attempt);

    public CreatorBuildTeam(ProjectLocation project, TabControl conversations, Func<string, bool> addToDraft,
        Action<string?> status, Action<CombinedCheck> checkedBuild, Func<(string Executable, string? Model)?> client)
    {
        this.project = project;
        this.conversations = conversations;
        this.addToDraft = addToDraft;
        this.status = status;
        this.checkedBuild = checkedBuild;
        this.client = client;
        store = new(project.Manifest, project.ProjectId);
    }

    public void UpdateConnection(bool connected)
    {
        foreach (var tab in tabs) tab.Pane.SetCanStart(connected && tab.Available);
    }

    public async Task<(CombinedCheck? Latest, CombinedCheck? Passing)> RestoreAsync(CancellationToken cancellation)
    {
        try
        {
            round = store.Read();
            if (round is null) return (null, null);
            if (round.Base is { } checkpoint)
            {
                var path = CheckpointPath(checkpoint);
                RequireDirectory(Path.GetDirectoryName(path)!);
                if (Hash(ReadFile(path, 4 * 1024 * 1024)) != checkpoint.Hash)
                    throw new InvalidDataException("The worker checkpoint changed.");
                var source = await ProjectFiles.Command(cancellation, "inspect", CheckpointSource(checkpoint));
                if (source.GetProperty("project_id").GetString() != project.ProjectId ||
                    source.GetProperty("revision").GetInt64() != checkpoint.Revision)
                    throw new InvalidDataException("The worker checkpoint belongs to a different revision or project.");
            }
            if (round.Workers.Any(worker => worker.Latest?.State == "running" || worker.Assigned?.State == "running") ||
                round.Batches.Any(Preparing) || round.Verifications?.Any(check => check.Pending) == true)
            {
                Save(round with
                {
                    Workers = round.Workers.Select(worker => worker with
                    {
                        Latest = Interrupt(worker.Latest), Assigned = Interrupt(worker.Assigned)
                    }).ToArray(),
                    Batches = round.Batches.Select(batch => Preparing(batch) ? batch with
                    {
                        Interrupted = true,
                        Error = "The workspace closed before preparing these workers."
                    } : batch).ToArray(),
                    Verifications = round.Verifications?.Select(check => check.Pending ? check with
                    {
                        Interrupted = true,
                        Error = "The workspace closed before verification completed. Submit a new request to check these drafts again."
                    } : check).ToArray(),
                    FollowUps = round.FollowUps?.Select(followup => followup with { Turn = Interrupt(followup.Turn)! }).ToArray()
                });
            }
            foreach (var worker in round.Workers)
            {
                string? problem = null;
                var available = false;
                try
                {
                    var current = await InspectWorkerAsync(worker, cancellation);
                    available = true;
                    if (worker.Latest?.WorldHash is { } hash && hash != current.GetProperty("world_sha256").GetString())
                        problem = "Draft changed after its report. Review the current scene in a follow-up.";
                }
                catch (Exception failure) when (!cancellation.IsCancellationRequested) { problem = failure.Message; }
                AddTab(worker, available, problem);
            }
            var latest = await RestoreCombinedAsync(round.Latest, cancellation);
            var passing = round.Passing == round.Latest ? latest : await RestoreCombinedAsync(round.Passing, cancellation);
            var proposed = round.Verifications?.FirstOrDefault(check => check.Id == round.Proposed)?.Result?.Build;
            var proposal = proposed == round.Latest ? latest : await RestoreCombinedAsync(proposed, cancellation);
            Proposal = proposal?.Attempt.Retained == true ? proposal : null;
            return (latest, passing?.Attempt.Retained == true ? passing : null);
        }
        catch
        {
            restoreFailed = true;
            throw;
        }
        finally { loaded = true; }
    }

    private static bool Preparing(Batch batch) => batch.Workers.Length == 0 && batch.Error is null && !batch.Interrupted;
    private static Turn? Interrupt(Turn? turn) => turn?.State == "running"
        ? turn with { State = "interrupted", Error = "Worker interrupted. Review its draft before sending a follow-up." } : turn;

    public async Task StartRoundAsync()
    {
        if (!CanStartRound) return;
        var next = store.NewRound();
        store.Write(next, replacing: round!.Id);
        round = next;
        Proposal = null;
        restoreFailed = false;
        conversations.SelectedIndex = 0;
        foreach (var tab in tabs)
        {
            tab.Pane.Enable(false);
            conversations.Items.Remove(tab.Tab);
        }
        var retiring = tabs.Select(tab => tab.Pane.ResetAsync()).ToArray();
        tabs.Clear();
        managedTurns.Clear();
        await Task.WhenAll(retiring);
    }

    private Round Current => round ?? throw new InvalidOperationException("There is no worker round.");
    private Checkpoint Base => Current.Base ?? throw new InvalidOperationException("There is no worker checkpoint.");
    private string RoundPath => store.DirectoryFor(Current.Id);
    private string CheckpointPath(Checkpoint checkpoint) => Path.Combine(RoundPath, checkpoint.Folder, "checkpoint.json");
    private string CheckpointSource(Checkpoint checkpoint) => Path.Combine(RoundPath, checkpoint.Folder, "source", checkpoint.Manifest);
    private string Manifest(Worker worker) => Path.Combine(RoundPath, worker.Folder, Base.Manifest);

    private void Save(Round next)
    {
        store.Write(next);
        round = next;
        var previous = changed;
        changed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        previous.TrySetResult();
    }

    public Task AddAsync()
    {
        if (!CanAdd) return Task.CompletedTask;
        return provision = AddCoreAsync();
    }

    private async Task AddCoreAsync()
    {
        adding = true;
        status("Preparing a worker draft…");
        try
        {
            if (round is null) Save(store.NewRound());
            await CaptureAsync(null);
            if (closing) return;
            var worker = await DraftAsync(Current.Workers.Length + 1);
            Save(Current with { Workers = [.. Current.Workers, worker] });
            if (!closing) conversations.SelectedItem = AddTab(worker, available: true);
            status(null);
        }
        catch (Exception failure) { if (!closing) status(failure.Message); }
        finally { adding = false; }
    }

    private async Task CaptureAsync(long? expectedRevision)
    {
        if (Current.Base is { } existing)
        {
            if (expectedRevision is not null && expectedRevision != existing.Revision)
                throw new InvalidOperationException($"This worker round uses revision {existing.Revision}. Start a new worker round to use the current scene.");
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
        var folder = Directory.Exists(Path.Combine(RoundPath, "base")) ? "base-" + Guid.NewGuid().ToString("N")[..8] : "base";
        var checkpoint = await ProjectFiles.Command("checkpoint", project.Manifest,
            "--output", Path.Combine(RoundPath, folder));
        if (closing) return;
        var revision = checkpoint.GetProperty("revision").GetInt64();
        var source = checkpoint.GetProperty("source_manifest").GetString()!;
        var captured = await ProjectFiles.Command("inspect", source);
        if (captured.GetProperty("project_id").GetString() != project.ProjectId ||
            expectedRevision is not null && expectedRevision != revision)
            throw new InvalidOperationException("The project changed while capturing the worker checkpoint. Check the current project and revision.");
        Save(Current with { Base = new(folder, Path.GetFileName(source), checkpoint.GetProperty("source_sha256").GetString()!, revision) });
    }

    private async Task<Worker> DraftAsync(int number)
    {
        var suffix = Guid.NewGuid().ToString("N")[..8];
        var folder = $"w{number}-{suffix}";
        var result = await ProjectFiles.Command("draft", CheckpointPath(Base),
            "--output", Path.Combine(RoundPath, folder), "--expected-base-sha256", Base.Hash);
        return new(folder, result.GetProperty("project_id").GetString()!);
    }

    private TabItem AddTab(Worker worker, bool available, string? problem = null)
    {
        var manifest = Manifest(worker);
        var checkpoint = Base;
        var checkpointPath = CheckpointPath(checkpoint);
        var name = "Worker " + (tabs.Count + 1);
        var engine = Path.Combine(AppContext.BaseDirectory, "poima.exe");
        var scope = $"You are editing Poima draft {worker.ProjectId}, forked from authored revision {checkpoint.Revision}. " +
            "Make the assigned change using the draft Poima tools. Verify the resulting draft revision and entities. " +
            "Use poima_discover and poima_reference for context. Report the changed entity IDs and verification results. " +
            "The lead reviews the pinned draft scene and verifies the assembled candidate.";
        var pane = new CreatorReviews.WorkerPane(name, "Assign part of the game to this worker.",
            cancellation =>
            {
                var provider = client() ?? throw new InvalidOperationException("Connect the main Codex chat to run this worker.");
                var validated = ValidateWorker(worker);
                return CodexReview.OpenDraftAsync(provider.Executable, engine, validated, provider.Model, cancellation);
            },
            () => scope, draftPath: manifest, useReport: report =>
            {
                if (closing) return;
                var message = $"{name} completed a task on an independent Poima draft.\n" +
                    $"The base is `{checkpointPath}` at authored revision {checkpoint.Revision}.\n" +
                    $"Its SHA-256 is `{checkpoint.Hash}`. The worker manifest is `{manifest}`.\n\n" +
                    "Review the draft, then use project inspect to get its world_sha256 for project integrate. " +
                    "Poima combines pinned scene edits and copies their cooked dependencies. Select other source files by hash. " +
                    "Build and verify the resulting candidate before offering it for play.\n\n" + report;
                status(addToDraft(message) ? "Worker report added to the main chat draft."
                    : "Wait for the main chat to finish before adding the report.");
            },
            recordStart: task => RememberStart(worker.ProjectId, task),
            recordFinish: result => RememberFinishAsync(worker.ProjectId, result));
        pane.SetCanStart(available && client() is not null);
        if (worker.Latest is { } turn)
            pane.Restore(turn.Task, problem is null ? turn.Report : null,
                problem ?? (turn.State == "completed" ? "Worker finished" : turn.Error ?? "Ready"));
        else if (problem is not null) pane.Restore("", null, problem);
        var tab = CreatorWindow.ConversationTab(name, pane.View);
        tabs.Add(new(pane, tab, available));
        conversations.Items.Add(tab);
        return tab;
    }

    private string ValidateWorker(Worker worker) =>
        CodexReview.ValidateDraft(Manifest(worker), worker.ProjectId, project.ProjectId, Base.Hash, Base.Revision);

    private async Task<JsonElement> InspectWorkerAsync(Worker worker, CancellationToken cancellation = default)
    {
        var info = await ProjectFiles.Command(cancellation, "inspect", ValidateWorker(worker));
        if (info.GetProperty("project_id").GetString() != worker.ProjectId)
            throw new InvalidDataException("The worker draft identity changed.");
        return info;
    }

    private Worker FindWorker(string id) => Current.Workers.Single(worker => worker.ProjectId == id);

    private void RememberStart(string id, string task)
    {
        var worker = FindWorker(id);
        var started = new Turn(task, "running");
        if (worker.Latest != started) UpdateWorker(worker with { Latest = started });
    }

    private async Task<CreatorReviews.WorkerPane.TurnResult> RememberFinishAsync(string id, CreatorReviews.WorkerPane.TurnResult result)
    {
        var worker = FindWorker(id);
        string? world = null;
        if (result.State == "completed")
        {
            try { world = (await InspectWorkerAsync(worker)).GetProperty("world_sha256").GetString(); }
            catch (Exception failure) { result = new("failed", null, "Couldn’t inspect the finished draft. " + failure.Message); }
        }
        var turn = new Turn(worker.Latest!.Task, result.State, result.Report, ShortError(result.Error), world);
        var updated = worker with { Latest = turn, Assigned = worker.Assigned?.State == "running" ? turn : worker.Assigned };
        Save(Current with
        {
            Workers = ReplacedWorker(updated),
            FollowUps = Current.FollowUps?.Select(followup => followup.DraftId == id && followup.Turn.State == "running"
                ? followup with { Turn = turn } : followup).ToArray()
        });
        return result;
    }

    private void UpdateWorker(Worker replacement) =>
        Save(Current with { Workers = ReplacedWorker(replacement) });

    private Worker[] ReplacedWorker(Worker replacement) =>
        Current.Workers.Select(worker => worker.ProjectId == replacement.ProjectId ? replacement : worker).ToArray();

    private static string? ShortError(string? value) => value is { Length: > 2000 } ? value[..2000] : value;
    private Batch? FindBatch(string id) => round?.Batches.FirstOrDefault(batch => batch.Id == id);
    private Verification? FindVerification(string id) => round?.Verifications?.FirstOrDefault(check => check.Id == id);
    private FollowUp? FindFollowUp(string id) => round?.FollowUps?.FirstOrDefault(followup => followup.Id == id);
    private static string PayloadHash(CodexWorkerRequest request) =>
        Hash(JsonSerializer.SerializeToUtf8Bytes(new { revision = request.ExpectedRevision, request.Tasks }));

    public async Task<JsonObject> CoordinateAsync(CodexWorkerRequest request, bool canStart,
        CancellationToken cancellation)
    {
        if (!loaded || restoreFailed) throw new InvalidOperationException("The worker round could not be loaded.");
        if (request.FollowUp is not null || FindFollowUp(request.RequestId) is not null)
            return await CoordinateFollowUpAsync(request, canStart, cancellation);
        if (request.Drafts is not null || FindVerification(request.RequestId) is not null)
            return await CoordinateVerificationAsync(request, canStart, cancellation);
        if (FindBatch(request.RequestId) is { } existing)
        {
            if (request.ExpectedRevision is not null && existing.PayloadHash != PayloadHash(request))
                throw new InvalidOperationException("This request_id already identifies a different assignment.");
            if (request.WaitMilliseconds > 0 && Pending(existing))
            {
                try { await changed.Task.WaitAsync(TimeSpan.FromMilliseconds(request.WaitMilliseconds), cancellation); }
                catch (TimeoutException) { }
            }
            return Describe(FindBatch(request.RequestId) ??
                throw new InvalidOperationException("The worker round changed."), replayed: request.ExpectedRevision is not null);
        }
        if (request.ExpectedRevision is not long revision) throw new InvalidOperationException("Unknown worker request. Inspect a request_id used with assign, revise or verify.");
        if (!CanAdd || (round?.Workers.Length ?? 0) + request.Tasks.Length > CodexWorkerTool.MaximumWorkers)
            throw new InvalidOperationException("Wait for draft preparation or assign within the workspace limit of ten workers.");
        if (round?.Batches.Length >= MaximumAssignments) throw new InvalidOperationException("Start a new worker round to assign more work.");
        var current = round ?? store.NewRound();
        var batch = new Batch(request.RequestId, revision, PayloadHash(request), []);
        Save(current with { Batches = [.. current.Batches, batch] });
        provision = StartBatchAsync(batch, request.Tasks);
        return Describe(FindBatch(batch.Id)!, replayed: false);
    }

    private async Task StartBatchAsync(Batch batch, string[] tasks)
    {
        adding = true;
        status("Preparing worker drafts…");
        try
        {
            await CaptureAsync(batch.Revision);
            if (closing) return;
            var staged = new List<Worker>();
            foreach (var task in tasks)
            {
                var worker = await DraftAsync(Current.Workers.Length + staged.Count + 1);
                if (closing) return;
                var turn = new Turn(task, "running");
                staged.Add(worker with { Latest = turn, Assigned = turn });
            }
            var prepared = batch with { Workers = staged.Select(worker => worker.ProjectId).ToArray() };
            Save(Current with { Workers = [.. Current.Workers, .. staged], Batches = Replaced(prepared) });
            foreach (var worker in staged)
            {
                AddTab(worker, available: true);
                managedTurns.Add(RunAssignedAsync(tabs[^1].Pane, worker));
            }
            status(null);
        }
        catch (Exception failure)
        {
            if (FindBatch(batch.Id) is { } saved && Preparing(saved)) TrySetBatchFailure(saved, failure.Message, interrupted: false);
            if (!closing) status(failure.Message);
        }
        finally
        {
            adding = false;
            if (closing && FindBatch(batch.Id) is { } saved && Preparing(saved))
                TrySetBatchFailure(saved, "The workspace closed during draft preparation.", interrupted: true);
        }
    }

    private async Task RunAssignedAsync(CreatorReviews.WorkerPane pane, Worker worker)
    {
        CreatorReviews.WorkerPane.TurnResult result;
        try { result = await pane.RunAsync(worker.Assigned!.Task); }
        catch (Exception failure) { result = new("failed", null, failure.Message); }
        try
        {
            if (FindWorker(worker.ProjectId).Assigned?.State == "running")
                await RememberFinishAsync(worker.ProjectId, result);
        }
        catch (Exception failure) { if (!closing) status(failure.Message); }
    }

    private Batch[] Replaced(Batch replacement) =>
        Current.Batches.Select(batch => batch.Id == replacement.Id ? replacement : batch).ToArray();

    private void TrySetBatchFailure(Batch batch, string error, bool interrupted)
    {
        try { Save(Current with { Batches = Replaced(batch with { Error = ShortError(error), Interrupted = interrupted }) }); }
        catch (Exception failure) { if (!closing) status(failure.Message); }
    }

    private bool Pending(Batch batch) => Preparing(batch) ||
        batch.Workers.Any(id => FindWorker(id).Assigned?.State == "running");

    private JsonObject Describe(Batch batch, bool replayed)
    {
        var rows = new JsonArray();
        var allCompleted = true;
        var interrupted = batch.Interrupted;
        foreach (var id in batch.Workers)
        {
            var worker = FindWorker(id);
            var turn = worker.Assigned!;
            allCompleted &= turn.State == "completed";
            interrupted |= turn.State == "interrupted";
            rows.Add(DescribeTurn(worker, turn));
        }
        var state = Preparing(batch) ? "preparing" : Pending(batch) ? "running" : interrupted ? "interrupted" :
            batch.Error is null && allCompleted ? "completed" : "failed";
        var result = new JsonObject { ["request_id"] = batch.Id, ["base_revision"] = batch.Revision,
            ["state"] = state, ["replayed"] = replayed, ["workers"] = rows };
        if (batch.Error is not null) result["error"] = batch.Error;
        return result;
    }

    private JsonObject DescribeTurn(Worker worker, Turn turn)
    {
        var number = Array.FindIndex(Current.Workers, item => item.ProjectId == worker.ProjectId) + 1;
        var row = new JsonObject { ["worker"] = "Worker " + number, ["draft_id"] = worker.ProjectId,
            ["manifest"] = Manifest(worker), ["state"] = turn.State };
        if (turn.Report is { } report)
        {
            row["report"] = Limit(report, 4000);
            if (report.Length > 4000) row["report_truncated"] = true;
            row["world_sha256"] = turn.WorldHash;
        }
        if (turn.Error is { } error) row["error"] = error;
        return row;
    }

    private async Task<JsonObject> CoordinateFollowUpAsync(CodexWorkerRequest request, bool canStart,
        CancellationToken cancellation)
    {
        if (request.ExpectedRevision is not null || request.Drafts is not null ||
            FindBatch(request.RequestId) is not null || FindVerification(request.RequestId) is not null)
            throw new InvalidOperationException("This request_id belongs to a different worker operation.");
        var followup = request.FollowUp;
        if (FindFollowUp(request.RequestId) is { } existing)
        {
            if (followup is not null && (followup.Draft != new CodexWorkerScene(existing.DraftId, existing.ExpectedWorldHash) ||
                    followup.Task != existing.Turn.Task))
                throw new InvalidOperationException("This request_id already identifies a different follow-up.");
            if (request.WaitMilliseconds > 0 && existing.Turn.State == "running")
            {
                try { await changed.Task.WaitAsync(TimeSpan.FromMilliseconds(request.WaitMilliseconds), cancellation); }
                catch (TimeoutException) { }
            }
            return Describe(FindFollowUp(request.RequestId) ??
                throw new InvalidOperationException("The worker round changed."), replayed: followup is not null);
        }
        if (!canStart || closing || adding || IsVerifying)
            throw new InvalidOperationException("Stop the game or wait for current project work before revising a draft.");
        var index = round is null ? -1 : Array.FindIndex(Current.Workers, worker => worker.ProjectId == followup!.Draft.DraftId);
        if (index < 0) throw new InvalidOperationException("The selected draft is outside the current worker round.");
        var tab = tabs[index];
        var worker = Current.Workers[index];
        if (!tab.Available || tab.Pane.Busy || worker.Latest?.State == "running")
            throw new InvalidOperationException("Wait for the worker to finish its current task.");
        var receipts = Current.FollowUps ?? [];
        if (receipts.Length >= MaximumAssignments)
            throw new InvalidOperationException("Start a new worker round to send more follow-ups.");
        cancellation.ThrowIfCancellationRequested();
        var receipt = new FollowUp(request.RequestId, worker.ProjectId, followup!.Draft.WorldSha256, new(followup.Task, "running"));
        Save(Current with { FollowUps = [.. receipts, receipt], Workers = ReplacedWorker(worker with { Latest = receipt.Turn }) });
        managedTurns.RemoveAll(turn => turn.IsCompleted);
        managedTurns.Add(RunFollowUpAsync(tab.Pane, worker, receipt));
        return Describe(FindFollowUp(receipt.Id)!, replayed: false);
    }

    private async Task RunFollowUpAsync(CreatorReviews.WorkerPane pane, Worker worker, FollowUp receipt)
    {
        CreatorReviews.WorkerPane.TurnResult result;
        try
        {
            result = await pane.RunAsync(receipt.Turn.Task, async cancellation =>
            {
                var state = await InspectWorkerAsync(worker, cancellation);
                if (state.GetProperty("world_sha256").GetString() != receipt.ExpectedWorldHash)
                    throw new InvalidOperationException("The draft changed. Inspect its current world_sha256 before sending a follow-up.");
            });
        }
        catch (Exception failure) { result = new("failed", null, failure.Message); }
        try
        {
            if (FindFollowUp(receipt.Id)?.Turn.State == "running")
                await RememberFinishAsync(worker.ProjectId, result);
        }
        catch (Exception failure) { if (!closing) status(failure.Message); }
    }

    private JsonObject Describe(FollowUp receipt, bool replayed)
    {
        var reply = DescribeTurn(FindWorker(receipt.DraftId), receipt.Turn);
        reply["request_id"] = receipt.Id;
        reply["expected_world_sha256"] = receipt.ExpectedWorldHash;
        reply["replayed"] = replayed;
        return reply;
    }

    private async Task<JsonObject> CoordinateVerificationAsync(CodexWorkerRequest request, bool canStart,
        CancellationToken cancellation)
    {
        if (request.ExpectedRevision is not null || FindBatch(request.RequestId) is not null)
            throw new InvalidOperationException("This request_id belongs to a different worker operation.");
        var hash = request.Drafts is { } drafts ? Hash(JsonSerializer.SerializeToUtf8Bytes(drafts)) : null;
        if (FindVerification(request.RequestId) is { } existing)
        {
            if (hash is not null && hash != existing.PayloadHash)
                throw new InvalidOperationException("This request_id already identifies a different verification.");
            if (request.WaitMilliseconds > 0 && existing.Pending)
            {
                try { await changed.Task.WaitAsync(TimeSpan.FromMilliseconds(request.WaitMilliseconds), cancellation); }
                catch (TimeoutException) { }
            }
            return Describe(FindVerification(request.RequestId) ??
                throw new InvalidOperationException("The worker round changed."), replayed: hash is not null);
        }
        if (!canStart || closing || adding || IsVerifying)
            throw new InvalidOperationException("Stop the game or wait for current project work before checking a combination.");
        if (!File.Exists(Path.Combine(ProjectFiles.PreviewRuntime, "runtime.json")))
            throw new InvalidOperationException("Install the Windows player to check a combined build.");
        RequireSuite();
        var receipts = Current.Verifications ?? [];
        if (receipts.Length >= MaximumAssignments)
            throw new InvalidOperationException("Start a new worker round to check more combinations.");

        var selected = request.Drafts!.Select(draft =>
        {
            var index = Array.FindIndex(Current.Workers, worker => worker.ProjectId == draft.DraftId);
            if (index < 0) throw new InvalidOperationException("A selected draft is outside the current worker round.");
            var pane = tabs[index].Pane;
            var worker = Current.Workers[index];
            if (pane.Busy || pane.Finding is not { } report || worker.Latest?.WorldHash != draft.WorldSha256)
                throw new InvalidOperationException($"Review the latest finished report for {pane.Name} before verifying its draft.");
            return new ReviewedDraft(pane.Name, Manifest(worker), Base.Revision, draft.WorldSha256, report);
        }).ToArray();
        cancellation.ThrowIfCancellationRequested();
        var receipt = new Verification(request.RequestId, hash!);
        Save(Current with { Verifications = [.. receipts, receipt] });
        verifying = new CancellationTokenSource();
        verificationWork = RunVerificationAsync(receipt, selected, verifying);
        return Describe(FindVerification(receipt.Id)!, replayed: false);
    }

    private async Task RunVerificationAsync(Verification receipt, ReviewedDraft[] selected, CancellationTokenSource source)
    {
        var cancellation = source.Token;
        string? finalMessage = null;
        try
        {
            status("Combining the selected scenes…");
            var (check, candidate) = await BuildCombinationAsync(selected, status, cancellation);
            var attempt = check.Attempt;
            using var report = await attempt.ReadReportAsync();
            var result = new VerificationResult(candidate, attempt.Passed, attempt.PassedGroups, attempt.TotalGroups,
                DescribeFailures(report.RootElement));
            cancellation.ThrowIfCancellationRequested();
            Save(Current with
            {
                Latest = candidate,
                Proposed = attempt.Passed ? receipt.Id : Current.Proposed,
                Verifications = ReplaceVerification(receipt with { Result = result })
            });
            if (attempt.Passed) Proposal = check;
            checkedBuild(check);
            finalMessage = "Combined game. " + attempt.Summary;
        }
        catch (Exception failure)
        {
            var message = cancellation.IsCancellationRequested ? "The workspace closed before verification completed." : failure.Message;
            try
            {
                Save(Current with { Verifications = ReplaceVerification(receipt with
                {
                    Error = ShortError(message), Interrupted = cancellation.IsCancellationRequested
                }) });
            }
            catch (Exception saveFailure) { message += " The receipt could not be saved. " + saveFailure.Message; }
            finalMessage = message;
        }
        finally
        {
            verifying = null;
            source.Dispose();
            if (!closing) status(finalMessage);
        }
    }

    private Verification[] ReplaceVerification(Verification replacement) =>
        Current.Verifications!.Select(check => check.Id == replacement.Id ? replacement : check).ToArray();

    private JsonObject Describe(Verification receipt, bool replayed)
    {
        var result = receipt.Result;
        var reply = new JsonObject
        {
            ["request_id"] = receipt.Id,
            ["state"] = receipt.Pending ? "running" : receipt.Interrupted ? "interrupted" : result?.Passed == true ? "passed" : "failed",
            ["replayed"] = replayed
        };
        if (receipt.Error is { } error) reply["error"] = error;
        if (result is null) return reply;
        reply["bundle_sha256"] = result.Build.Bundle;
        reply["snapshot_sha256"] = result.Build.Snapshot;
        reply["report_sha256"] = result.Build.ReportHash;
        reply["groups_passed"] = result.PassedGroups;
        reply["groups_total"] = result.TotalGroups;
        reply["awaiting_review"] = Current.Proposed == receipt.Id;
        reply["playable"] = Current.Passing == result.Build;
        reply["failures"] = JsonSerializer.SerializeToNode(result.Failures.Select(failure => new
        {
            group = failure.Group, check = failure.Check, tick = failure.Tick, issue = failure.Issue
        }));
        return reply;
    }

    private static FailedCheck[] DescribeFailures(JsonElement report)
    {
        var failures = new List<FailedCheck>();
        foreach (var group in report.GetProperty("groups").EnumerateArray())
        {
            if (group.GetProperty("passed").GetBoolean()) continue;
            var name = Limit(group.GetProperty("name").GetString()!, 256);
            if (group.TryGetProperty("issue", out var issue))
                failures.Add(new(name, null, null, Limit(issue.GetProperty("message").GetString()!, 512)));
            else
            {
                var first = group.GetProperty("checks").EnumerateArray().FirstOrDefault(check => !check.GetProperty("passed").GetBoolean());
                failures.Add(first.ValueKind == JsonValueKind.Object
                    ? new(name, Limit(first.GetProperty("label").GetString()!, 256), first.GetProperty("tick").GetInt64(), null)
                    : new(name, null, null, null));
            }
            if (failures.Count == MaximumVerificationFailures) break;
        }
        return [.. failures];
    }

    private static string Limit(string value, int length) => value.Length <= length ? value : value[..length];

    private void RequireSuite()
    {
        if (!File.Exists(Path.Combine(Path.GetDirectoryName(CheckpointSource(Base))!, ProjectFiles.VerificationSuiteName)))
            throw new InvalidOperationException("This worker round started without a playtest suite. Add game.verify.json and start a new worker round.");
    }

    public async Task<ReviewedDraft[]> ReviewAsync(CancellationToken cancellation)
    {
        RequireSuite();

        var result = new List<ReviewedDraft>();
        for (var index = 0; index < tabs.Count; index++)
        {
            var pane = tabs[index].Pane;
            var worker = Current.Workers[index];
            if (pane.Busy || pane.Finding is not { } report || worker.Latest?.WorldHash is not { } hash) continue;
            var draft = await InspectWorkerAsync(worker, cancellation);
            if (pane.Busy || pane.Finding != report) continue;
            if (draft.GetProperty("world_sha256").GetString() != hash)
            {
                pane.Restore(worker.Latest.Task, null, "Draft changed after its report. Review the current scene in a follow-up.");
                continue;
            }
            result.Add(new(pane.Name, Manifest(worker), draft.GetProperty("revision").GetInt64(), hash, report));
        }
        return [.. result];
    }

    public async Task<CombinedCheck> CombineAsync(ReviewedDraft[] selected, Action<string> setProgress,
        CancellationToken cancellation)
    {
        var (check, candidate) = await BuildCombinationAsync(selected, setProgress, cancellation);
        Save(Current with { Latest = candidate, Passing = check.Attempt.Retained ? candidate : Current.Passing });
        return check;
    }

    public async Task<CombinedCheck> AcceptAsync(CombinedCheck reviewed, CancellationToken cancellation)
    {
        if (IsVerifying || reviewed != Proposal || Current.Proposed is not { } id ||
            FindVerification(id)?.Result is not { Passed: true } result)
            throw new InvalidOperationException("The proposed build changed. Review the current results.");
        var checkedBuild = await RestoreCombinedAsync(result.Build, cancellation);
        if (checkedBuild != reviewed || !checkedBuild.Attempt.Retained)
            throw new InvalidOperationException("The proposed build or its playtest report changed.");
        cancellation.ThrowIfCancellationRequested();
        Save(Current with { Passing = result.Build, Proposed = null });
        Proposal = null;
        return checkedBuild;
    }

    private async Task<(CombinedCheck Check, Candidate Build)> BuildCombinationAsync(ReviewedDraft[] selected,
        Action<string> setProgress, CancellationToken cancellation)
    {
        var captured = Base;
        if (selected.Length == 0 || !selected.All(draft => Current.Workers.Select((worker, index) => (worker, pane: tabs[index].Pane))
                .Any(item => Manifest(item.worker) == draft.Manifest && item.worker.Latest?.WorldHash == draft.WorldHash &&
                    !item.pane.Busy && item.pane.Finding == draft.Report)))
            throw new InvalidOperationException("A selected worker has changed its report. Review the drafts again.");

        var candidateFolder = "combined-" + Guid.NewGuid().ToString("N")[..12];
        var root = Directory.CreateDirectory(Path.Combine(RoundPath, candidateFolder)).FullName;
        var request = Path.Combine(root, "selection.json");
        await File.WriteAllTextAsync(request, JsonSerializer.Serialize(new
        {
            base_sha256 = captured.Hash,
            drafts = selected.Select(draft => new { manifest = draft.Manifest, world_sha256 = draft.WorldHash, files = Array.Empty<string>() })
        }), cancellation);
        var merged = await ProjectFiles.Command("integrate", CheckpointPath(captured),
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

        var check = ReadCombined(projectId, history, entry);
        var candidate = new Candidate(candidateFolder, projectId, check.Snapshot, check.Attempt.Build, check.Attempt.Sha256);
        return (check, candidate);
    }

    private async Task<CombinedCheck?> RestoreCombinedAsync(Candidate? candidate, CancellationToken cancellation)
    {
        if (candidate is null) return null;
        try
        {
            var history = Path.Combine(RoundPath, candidate.Folder, ".poima-history", candidate.ProjectId);
            RequireDirectory(history);
            var versions = await ProjectFiles.Command(cancellation, "versions", history, "--project-id", candidate.ProjectId);
            var entry = versions.GetProperty("entries").EnumerateArray().SingleOrDefault();
            if (entry.ValueKind != JsonValueKind.Object || entry.GetProperty("snapshot_sha256").GetString() != candidate.Snapshot ||
                entry.GetProperty("bundle_sha256").GetString() != candidate.Bundle ||
                candidate.ReportHash is { } hash && entry.GetProperty("report_sha256").GetString() != hash)
                throw new InvalidDataException("A saved combined build or verification report changed.");
            return ReadCombined(candidate.ProjectId, history, entry);
        }
        catch (Exception failure) when (!cancellation.IsCancellationRequested)
        {
            status(failure.Message);
            return null;
        }
    }

    private static CombinedCheck ReadCombined(string projectId, string history, JsonElement entry) =>
        new(projectId, history, entry.GetProperty("checkpoint").GetString()!,
            entry.GetProperty("snapshot_sha256").GetString()!, entry.GetProperty("game_manifest").GetString()!,
            PlaytestAttempt.FromHistory(entry));

    public Task RetireAsync() => retirement ??= RetireCoreAsync();

    private async Task RetireCoreAsync()
    {
        closing = true;
        verifying?.Cancel();
        foreach (var tab in tabs) tab.Pane.Enable(false);
        if (provision is { } pending) await pending;
        if (verificationWork is { } checking) await checking;
        await Task.WhenAll(tabs.Select(tab => tab.Pane.ResetAsync()));
        await Task.WhenAll(managedTurns);
    }
}
