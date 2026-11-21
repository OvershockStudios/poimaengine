// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Poima.Editor;

// The pipe reader publishes only the newest sample. The creating UI thread
// owns the process lifecycle and publishes a bounded copy to the bridge.
internal sealed class VerifiedPlayback(string bundleHash, long revision)
{
    internal const string ProgressPrefix = "POIMA_VERIFIED_PROGRESS ";
    private sealed record Progress(long Tick, long Frames, bool? Focused, bool? Paused);

    private readonly string id = Guid.NewGuid().ToString("N");
    private Progress? pending, observed;
    private string state = "checking";
    private string? stopReason;
    private int? processId;
    private (string Stage, uint? Code, string Message)? failure;
    private bool changed = true;

    public string BundleHash { get; } = bundleHash;

    public void Receive(ReadOnlyMemory<char> json)
    {
        if (json.Length > 1024) return;
        try
        {
            using var document = JsonDocument.Parse(json);
            var value = document.RootElement;
            if (value.GetProperty("bundle_sha256").GetString() != BundleHash || value.GetProperty("revision").GetInt64() != revision)
                return;
            var tick = value.GetProperty("tick").GetInt64();
            var frames = value.GetProperty("frames_presented").GetInt64();
            const long maximum = 9007199254740991;
            if (tick is < 0 or > maximum || frames is < 0 or > maximum) return;
            Interlocked.Exchange(ref pending, new Progress(tick, frames, Flag("focused"), Flag("paused")));

            bool? Flag(string key) => value.GetProperty(key) is { ValueKind: JsonValueKind.Null } ? null : value.GetProperty(key).GetBoolean();
        }
        catch (Exception failure) when (failure is JsonException or KeyNotFoundException or InvalidOperationException or FormatException)
        { /* An unusable progress line is never a playback observation. */ }
    }

    public void Started(int pid) { processId = pid; state = "running"; changed = true; }

    public void Ended(string outcome, Exception? error = null)
    {
        Consume();
        if (error is not null)
        {
            var native = error as NativeCommandFailure;
            var message = error.Message.Trim();
            failure = (native?.Stage ?? "workspace", native?.ExitCode,
                message.Length > 256 ? message[..256] : message);
        }
        state = outcome;
        changed = true;
    }

    public void Closed(JsonElement played)
    {
        observed = new(played.GetProperty("tick").GetInt64(), played.GetProperty("frames_presented").GetInt64(), null, null);
        stopReason = played.GetProperty("stop_reason").GetString();
        state = "closed";
        changed = true;
    }

    private void Consume()
    {
        var sample = Interlocked.Exchange(ref pending, null);
        if (sample is null || (observed is not null && sample.Frames < observed.Frames)) return;
        observed = sample;
        changed = true;
    }

    public JsonObject? TakeSnapshot()
    {
        if (state == "running") Consume();
        if (!changed) return null;
        changed = false;
        var snapshot = new JsonObject
        {
            ["play_id"] = id, ["bundle_sha256"] = BundleHash, ["revision"] = revision, ["state"] = state,
            ["observation"] = observed is { } sample ? new JsonObject
            {
                ["tick"] = sample.Tick, ["frames_presented"] = sample.Frames,
                ["focused"] = sample.Focused, ["paused"] = sample.Paused
            } : null,
            ["stop_reason"] = stopReason
        };
        if (processId is int pid) snapshot["process_id"] = pid;
        if (failure is { } details)
            snapshot["failure"] = new JsonObject
            {
                ["stage"] = details.Stage, ["exit_code"] = details.Code, ["message"] = details.Message
            };
        return snapshot;
    }
}
