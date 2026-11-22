// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.ComponentModel;
using System.Text;
using System.Text.Json;

namespace Poima.Editor;

internal sealed record ProjectLocation(string Manifest, string Name, string World, string Root, string ProjectId, string Endpoint,
    long Revision, string Camera, string? InputProfile, bool Audio, JsonElement? Gameplay);

internal sealed class NativeCommandFailure(string stage, uint? exitCode, string message, Exception? inner = null)
    : InvalidOperationException(message, inner)
{
    public string Stage { get; } = stage;
    public uint? ExitCode { get; } = exitCode;
}

internal static class ProjectFiles
{
    public const string Extension = ".poimaproject";
    public const string DefaultManifestName = "project" + Extension;
    public const string LegacyManifestName = "project.json";
    public const string VerificationSuiteName = "game.verify.json";
    public static string PreviewSample => Path.Combine(AppContext.BaseDirectory, "samples", "Collection Room", "game.json");
    public static string PreviewRuntime => Path.Combine(Path.GetDirectoryName(PreviewSample)!, "runtime");

    public static bool IsManifest(string path) =>
        string.Equals(Path.GetExtension(path), Extension, StringComparison.OrdinalIgnoreCase) ||
        string.Equals(Path.GetFileName(path), LegacyManifestName, StringComparison.OrdinalIgnoreCase);

    public static async Task<ProjectLocation> Open(string manifest)
    {
        var project = await Command("inspect", Path.GetFullPath(manifest));
        var path = project.GetProperty("manifest").GetString()!;
        var root = Path.GetDirectoryName(path)!;
        var entry = project.GetProperty("entry");
        var world = Path.GetFullPath(entry.GetProperty("world").GetString()!, root);
        var projectId = project.GetProperty("project_id").GetString()!;
        return new(path, project.GetProperty("name").GetString()!, world, root, projectId,
            "poima-" + projectId, project.GetProperty("revision").GetInt64(),
            entry.GetProperty("camera").GetString()!, project.GetProperty("input_profile").GetString(),
            project.GetProperty("audio").GetBoolean(), project.TryGetProperty("gameplay", out var gameplay) ? gameplay : null);
    }

    public static Process Start(string executable, params string[] arguments) => Start(executable, null, arguments);

    public static Process Start(string executable, Encoding? outputEncoding, params string[] arguments) =>
        Process.Start(StartInfo(executable, outputEncoding, arguments)) ?? throw new InvalidOperationException("Poima could not start.");

    private static ProcessStartInfo StartInfo(string executable, Encoding? outputEncoding, string[] arguments)
    {
        var info = new ProcessStartInfo(executable)
        {
            WorkingDirectory = AppContext.BaseDirectory, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = outputEncoding
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return info;
    }

    public static Task<JsonElement> Command(params string[] arguments) =>
        Command(default, arguments);

    public static Task<JsonElement> Command(CancellationToken cancellation, params string[] arguments) =>
        RunCommand(Path.Combine(AppContext.BaseDirectory, "poima.exe"), ["project", .. arguments], cancellation: cancellation);

    internal static Task<JsonElement> InspectGameAsync(string manifest, string bundle, CancellationToken cancellation) =>
        RunCommand(Path.Combine(AppContext.BaseDirectory, "poima.exe"),
            ["game", "inspect", manifest, "--expected-bundle-sha256", bundle], cancellation: cancellation);

    public static async Task<JsonElement> VerifyExport(string bundle, string suite, string report, CancellationToken cancellation)
    {
        var manifest = Path.Combine(bundle, "game.json");
        using var stage = await GameChildWorkspace.PrepareAsync(manifest, suite, report, null, cancellation);
        var game = stage?.Manifest ?? manifest;
        var arguments = new List<string> { "game", "verify", game, "--suite", stage?.Suite ?? suite, "--report", stage?.Report ?? report };
        if (stage is not null) arguments.AddRange(["--expected-bundle-sha256", stage.BundleHash]);
        var result = await RunCommand(GameExecutable(game), [.. arguments], allowFailedVerification: true, cancellation: cancellation);
        if (stage is not null) await stage.PublishReportAsync(report, cancellation);
        return result;
    }

    public static async Task<JsonElement> PlayExport(string manifest, string saves, int gpu, VerifiedPlayback playback,
        CancellationToken preparation, Action launched)
    {
        using var stage = await GameChildWorkspace.PrepareAsync(manifest, null, null, playback.BundleHash, preparation);
        preparation.ThrowIfCancellationRequested();
        manifest = stage?.Manifest ?? manifest;
        var arguments = new List<string> { "game", "run", manifest, "--save-root", saves };
        if (gpu >= 0)
            arguments.AddRange(["--gpu", gpu.ToString(System.Globalization.CultureInfo.InvariantCulture)]);
        return await RunCommand(GameExecutable(manifest), [.. arguments], playback: playback, launched: launched);
    }

    private static string GameExecutable(string manifest) => Path.Combine(Path.GetDirectoryName(manifest)!, "runtime", "bin", "poima.exe");

    private static async Task<string> ReadPlaybackOutput(StreamReader reader, VerifiedPlayback playback)
    {
        var result = new StringBuilder();
        while (await reader.ReadLineAsync().ConfigureAwait(false) is { } line)
        {
            if (line.StartsWith(VerifiedPlayback.ProgressPrefix, StringComparison.Ordinal))
                playback.Receive(line.AsMemory(VerifiedPlayback.ProgressPrefix.Length));
            else result.AppendLine(line);
        }
        return result.ToString();
    }

    private static async Task<JsonElement> RunCommand(string executable, string[] arguments,
        bool allowFailedVerification = false, CancellationToken cancellation = default, VerifiedPlayback? playback = null,
        Action? launched = null)
    {
        var info = StartInfo(executable, playback is null ? null : Encoding.UTF8, arguments);
        if (playback is not null) info.Environment["POIMA_VERIFIED_PLAY_SHA256"] = playback.BundleHash;
        using var process = Launch(info);
        playback?.Started(process.Id);
        launched?.Invoke();
        var output = playback is null ? process.StandardOutput.ReadToEndAsync() : ReadPlaybackOutput(process.StandardOutput, playback);
        var error = process.StandardError.ReadToEndAsync();
        try { await process.WaitForExitAsync(cancellation); }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            try { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) when (process.HasExited) { }
            await process.WaitForExitAsync();
            await Task.WhenAll(output, error);
            throw;
        }
        var body = await output;
        var stderr = await error;
        var code = unchecked((uint)process.ExitCode);
        if (string.IsNullOrWhiteSpace(body))
            throw InvalidResult(code, stderr, "Poima returned no result.");
        try
        {
            using var response = JsonDocument.Parse(body);
            var root = response.RootElement;
            var result = root.GetProperty("result");
            if (code != 0 && !(allowFailedVerification && code == 4 &&
                result.ValueKind == JsonValueKind.Object && result.GetProperty("passed").ValueKind == JsonValueKind.False))
            {
                var message = root.GetProperty("diagnostics")[0].GetProperty("message").GetString() ?? "Poima reported a native error.";
                throw new NativeCommandFailure("command", code, $"{message} (exit 0x{code:X8})");
            }
            if (result.ValueKind != JsonValueKind.Object) throw new JsonException("Expected a project result object.");
            return result.Clone();
        }
        catch (Exception failure) when ((failure is JsonException or KeyNotFoundException or InvalidOperationException or IndexOutOfRangeException)
            && failure is not NativeCommandFailure)
        {
            throw InvalidResult(code, stderr, "Poima returned an invalid result.", failure);
        }
    }

    private static Process Launch(ProcessStartInfo info)
    {
        try { return Process.Start(info) ?? throw new NativeCommandFailure("launch", null, "Poima could not start."); }
        catch (Win32Exception error) { throw new NativeCommandFailure("launch", null, $"Poima could not start (Windows error {error.NativeErrorCode}). {error.Message}", error); }
    }

    private static NativeCommandFailure InvalidResult(uint code, string stderr, string reason, Exception? inner = null)
    {
        var detail = stderr.Trim();
        if (detail.Length > 512) detail = detail[^512..];
        return new("result", code, $"{reason} (exit 0x{code:X8})" + (detail.Length == 0 ? "" : " " + detail), inner);
    }
}
