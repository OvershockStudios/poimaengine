// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Poima.AgentHost;

namespace Poima.Editor;

internal sealed class CreatorWorkerRoundStore
{
    public sealed record Checkpoint(string Folder, string Manifest, string Hash, long Revision);
    public sealed record Turn(string Task, string State, string? Report = null, string? Error = null, string? WorldHash = null);
    public sealed record Worker(string Folder, string ProjectId, Turn? Latest = null, Turn? Assigned = null);
    public sealed record Batch(string Id, long Revision, string PayloadHash, string[] Workers, string? Error = null, bool Interrupted = false);
    public sealed record Candidate(string Folder, string ProjectId, string Snapshot, string Bundle, string? ReportHash = null);
    public sealed record FailedCheck(string Group, string? Check, long? Tick, string? Issue);
    public sealed record VerificationResult(Candidate Build, bool Passed, int PassedGroups, int TotalGroups, FailedCheck[] Failures);
    public sealed record Verification(string Id, string PayloadHash, VerificationResult? Result = null, string? Error = null,
        bool Interrupted = false)
    {
        [JsonIgnore]
        public bool Pending => Result is null && Error is null;
    }
    public sealed record Round(int Version, string Owner, string Id, Checkpoint? Base, Worker[] Workers, Batch[] Batches,
        Candidate? Latest = null, Candidate? Passing = null, Verification[]? Verifications = null, string? Proposed = null);

    public const int MaximumAssignments = 64;
    public const int MaximumVerificationFailures = 12;
    private const int MaximumBytes = 4 * 1024 * 1024;
    private const long MaximumRevision = 9007199254740991L;
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        MaxDepth = 12,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower
    };
    private readonly string directory, owner, pointer;

    public CreatorWorkerRoundStore(string manifest, string projectId)
    {
        var path = Path.GetFullPath(manifest);
        var project = Path.GetDirectoryName(path)!;
        var parent = Path.GetDirectoryName(project);
        directory = parent is null ? "" : Path.Combine(parent, ".poima-drafts");
        if (OperatingSystem.IsWindows()) path = path.ToUpperInvariant();
        owner = Hash(Encoding.UTF8.GetBytes(projectId + "\n" + path));
        pointer = Path.Combine(directory, owner + ".current");
    }

    public string DirectoryFor(string roundId) => Path.Combine(directory, roundId);
    public Round NewRound() => new(1, owner, Guid.NewGuid().ToString("N")[..12], null, [], []);

    public Round? Read()
    {
        if (directory.Length == 0 || !File.Exists(pointer)) return null;
        RequireDirectory(directory);
        var id = Encoding.ASCII.GetString(ReadFile(pointer, 12));
        if (!Hex(id, 12)) throw new InvalidDataException("The worker round pointer is invalid.");
        var folder = DirectoryFor(id);
        RequireDirectory(folder);
        try
        {
            using var document = JsonDocument.Parse(ReadFile(Path.Combine(folder, "round.json"), MaximumBytes),
                new JsonDocumentOptions { MaxDepth = 12, AllowDuplicateProperties = false });
            var result = document.RootElement.Deserialize<Round>(JsonOptions);
            if (result is null || result.Id != id) throw new InvalidDataException("The saved worker round has a different identity.");
            Validate(result);
            return result;
        }
        catch (JsonException failure) { throw new InvalidDataException("The saved worker round contains invalid JSON.", failure); }
    }

    public void Write(Round round, string? replacing = null)
    {
        Validate(round);
        if (directory.Length == 0) throw new IOException("The project needs a parent folder for worker drafts.");
        RequireDirectory(Path.GetDirectoryName(directory)!);
        Directory.CreateDirectory(directory);
        RequireDirectory(directory);
        var folder = DirectoryFor(round.Id);
        Directory.CreateDirectory(folder);
        RequireDirectory(folder);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(round, JsonOptions);
        if (bytes.Length > MaximumBytes) throw new InvalidDataException("The worker round exceeds its 4 MiB limit.");
        var selected = File.Exists(pointer) ? Encoding.ASCII.GetString(ReadFile(pointer, 12)) : null;
        if (selected is not null && selected != round.Id && selected != replacing)
            throw new InvalidOperationException("The active worker round changed. Reopen the current project.");
        WriteAtomic(Path.Combine(folder, "round.json"), bytes);
        if (selected != round.Id) WriteAtomic(pointer, Encoding.ASCII.GetBytes(round.Id));
    }

    private void Validate(Round round)
    {
        if (round.Version != 1 || round.Owner != owner || !Hex(round.Id, 12) ||
            round.Workers is null || round.Workers.Length > CodexWorkerTool.MaximumWorkers ||
            round.Batches is null || round.Batches.Length > MaximumAssignments ||
            round.Verifications is { Length: > MaximumAssignments } ||
            !ValidCandidate(round.Latest) || !ValidCandidate(round.Passing))
            throw new InvalidDataException("The saved worker round is invalid for this project.");
        var checkpoint = round.Base;
        if (checkpoint is null ? round.Workers.Length != 0 || round.Latest is not null || round.Passing is not null ||
            round.Verifications is { Length: > 0 } :
            !Leaf(checkpoint.Folder) || !ProjectName(checkpoint.Manifest) || !Hex(checkpoint.Hash, 64) || !Revision(checkpoint.Revision))
            throw new InvalidDataException("The saved worker checkpoint is invalid.");

        var ids = new HashSet<string>(StringComparer.Ordinal);
        var folders = new HashSet<string>(StringComparer.Ordinal);
        foreach (var worker in round.Workers)
            if (worker is null || !Hex(worker.ProjectId, 32) || !Leaf(worker.Folder) ||
                !ids.Add(worker.ProjectId) || !folders.Add(worker.Folder) || !ValidTurn(worker.Latest) || !ValidTurn(worker.Assigned))
                throw new InvalidDataException("The saved round contains an invalid worker.");
        var batches = new HashSet<string>(StringComparer.Ordinal);
        var assigned = new HashSet<string>(StringComparer.Ordinal);
        foreach (var batch in round.Batches)
        {
            if (batch is null || !Hex(batch.Id, 32) || !batches.Add(batch.Id) || !Revision(batch.Revision) || !Hex(batch.PayloadHash, 64) ||
                batch.Error is { Length: > 2000 } || batch.Workers is null || batch.Workers.Length > CodexWorkerTool.MaximumWorkers ||
                batch.Workers.Any(id => id is null || !ids.Contains(id) || !assigned.Add(id)))
                throw new InvalidDataException("The saved round contains an invalid assignment.");
        }
        if (round.Workers.Any(worker => assigned.Contains(worker.ProjectId) != (worker.Assigned is not null)))
            throw new InvalidDataException("A worker's assigned turn does not match its receipt.");
        foreach (var verification in round.Verifications ?? [])
        {
            if (verification is null || !Hex(verification.Id, 32) || !batches.Add(verification.Id) ||
                !Hex(verification.PayloadHash, 64) || verification.Error is { Length: > 2000 } ||
                (verification.Result is { } result ? !ValidResult(result) || verification.Error is not null || verification.Interrupted :
                    verification.Interrupted && verification.Error is null))
                throw new InvalidDataException("The saved round contains an invalid verification.");
        }
        if (round.Proposed is not null && round.Verifications?.Any(check =>
                check.Id == round.Proposed && check.Result?.Passed == true) != true)
            throw new InvalidDataException("The proposed build is missing its passing verification.");
    }

    private static bool ValidTurn(Turn? turn) => turn is null ||
        turn.Task is { Length: > 0 and <= CodexWorkerTool.MaximumTaskLength } &&
        turn.State is ("running" or "completed" or "failed" or "stopped" or "interrupted") &&
        turn.Error is not { Length: > 2000 } &&
        (turn.State == "completed" ? turn.Report is { Length: > 0 and <= CodexReview.MaximumFindingLength } && Hex(turn.WorldHash, 64) :
            turn.Report is null && turn.WorldHash is null);

    private static bool ValidCandidate(Candidate? value) => value is null ||
        Leaf(value.Folder) && Hex(value.ProjectId, 32) && Hex(value.Snapshot, 64) && Hex(value.Bundle, 64) &&
        (value.ReportHash is null || Hex(value.ReportHash, 64));
    private static bool ValidResult(VerificationResult result) =>
        result.Build is { ReportHash: not null } && ValidCandidate(result.Build) &&
        result.TotalGroups > 0 && result.PassedGroups >= 0 && result.PassedGroups <= result.TotalGroups &&
        result.Passed == (result.PassedGroups == result.TotalGroups) && result.Failures is { Length: <= MaximumVerificationFailures } &&
        result.Failures.All(failure => failure is not null && failure.Group is { Length: > 0 and <= 256 } &&
            failure.Check is not { Length: > 256 } && failure.Issue is not { Length: > 512 });
    private static bool Revision(long value) => value is >= 0 and <= MaximumRevision;
    internal static bool Hex(string? value, int count) => value is not null && value.Length == count &&
        value.All(c => c is (>= '0' and <= '9') or (>= 'a' and <= 'f'));
    private static bool Leaf(string? value) => value is { Length: > 0 and <= 32 } &&
        value.All(c => c is (>= 'a' and <= 'z') or (>= '0' and <= '9') or '-');
    private static bool ProjectName(string? value) => value is { Length: >= 1 and <= 128 } &&
        value.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.' or ' ') &&
        (value.EndsWith(".poimaproject", StringComparison.OrdinalIgnoreCase) ||
            value.Equals("project.json", StringComparison.OrdinalIgnoreCase));

    internal static string Hash(byte[] content) => Convert.ToHexStringLower(SHA256.HashData(content));

    internal static void RequireDirectory(string path)
    {
        for (DirectoryInfo? folder = new(path); folder is not null; folder = folder.Parent)
            if (!folder.Exists || folder.Attributes.HasFlag(FileAttributes.ReparsePoint))
                throw new IOException("Worker storage needs an ordinary directory.");
    }

    internal static byte[] ReadFile(string path, int maximum)
    {
        if ((File.GetAttributes(path) & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0)
            throw new IOException("Expected a regular worker record.");
        using var stream = File.OpenRead(path);
        var length = stream.Length;
        if (length < 1 || length > maximum) throw new InvalidDataException("The worker record has an invalid size.");
        var bytes = new byte[(int)length];
        stream.ReadExactly(bytes);
        if (stream.ReadByte() != -1) throw new IOException("The worker record changed during the read.");
        return bytes;
    }

    private static void WriteAtomic(string path, byte[] bytes)
    {
        if (Path.Exists(path) && (File.GetAttributes(path) & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0)
            throw new IOException("Expected an ordinary worker record destination.");
        var temporary = Path.Combine(Path.GetDirectoryName(path)!, ".pending-" + Guid.NewGuid().ToString("N"));
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: true);
        }
        finally { File.Delete(temporary); }
    }
}
