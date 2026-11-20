// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.IO.Compression;
using System.Text.Json;

namespace Poima.Editor;

internal static class WindowsClaudeInstall
{
    internal const string Version = "0.106.0";
    internal const long ArchiveBytes = 219_220_423;
    internal const string ArchiveSha256 = "5de09fe80ac78260eb56964b92e698bd4c467092f0c88e042438a8e05974903c";
    internal const string PackageUrl = $"https://github.com/agentclientprotocol/claude-agent-acp/releases/download/v{Version}/claude-agent-acp-windows-x64.zip";
    internal const string GuideUrl = "https://github.com/agentclientprotocol/claude-agent-acp#readme";
    private const string AdapterName = "claude-agent-acp.exe";
    private const string CliName = "claude.exe";
    private static readonly IReadOnlyDictionary<string, long> Files = new Dictionary<string, long>(StringComparer.Ordinal)
    {
        [AdapterName] = 118_511_616,
        [CliName] = 346_496_160
    };

    public static WindowsAgentPackage Package { get; } = new("Claude Code", "claude", Version, PackageUrl,
        ArchiveBytes, ArchiveSha256, ".poima-claude-installed", AdapterName, Files, UnpackAsync);

    public static string? ManagedCli(string? selectedAdapter)
    {
        var installed = Package.Executable(Package.Root);
        return installed is not null && string.Equals(installed, selectedAdapter, StringComparison.OrdinalIgnoreCase)
            ? Path.Combine(Path.GetDirectoryName(installed)!, CliName) : null;
    }

    public static async Task<bool> IsSignedInAsync(string cli, CancellationToken cancellation)
    {
        var info = Command(cli, interactive: false, "status");
        info.RedirectStandardOutput = info.RedirectStandardError = true;
        using var process = Start(info);
        try
        {
            var output = process.StandardOutput.ReadToEndAsync(cancellation);
            var discarded = process.StandardError.BaseStream.CopyToAsync(Stream.Null, cancellation);
            await Task.WhenAll(output, discarded, process.WaitForExitAsync(cancellation)).ConfigureAwait(false);
            using var reply = JsonDocument.Parse(output.Result);
            if (reply.RootElement.ValueKind == JsonValueKind.Object &&
                reply.RootElement.TryGetProperty("loggedIn", out var loggedIn) &&
                loggedIn.ValueKind is JsonValueKind.True or JsonValueKind.False &&
                process.ExitCode == (loggedIn.GetBoolean() ? 0 : 1))
                return loggedIn.GetBoolean();
        }
        catch (JsonException) { }
        catch (OperationCanceledException)
        {
            try { process.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) when (process.HasExited) { }
            await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
            throw;
        }
        throw new IOException("Poima couldn’t check Claude Code sign-in. Try again or open the setup guide.");
    }

    public static async Task<bool> SignInAsync(string cli, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        using var process = Start(Command(cli, interactive: true, "login"));
        await process.WaitForExitAsync(cancellation).ConfigureAwait(false);
        return process.ExitCode == 0;
    }

    private static ProcessStartInfo Command(string cli, bool interactive, string action)
    {
        var info = new ProcessStartInfo(cli)
        {
            UseShellExecute = interactive, CreateNoWindow = !interactive, WorkingDirectory = Path.GetDirectoryName(cli)!
        };
        info.ArgumentList.Add("auth");
        info.ArgumentList.Add(action);
        return info;
    }

    private static Process Start(ProcessStartInfo info) =>
        Process.Start(info) ?? throw new IOException("Windows couldn’t start Claude Code.");

    private static async Task UnpackAsync(Stream archive, string payload, CancellationToken cancellation)
    {
        using var zip = new ZipArchive(archive, ZipArchiveMode.Read, leaveOpen: true);
        if (zip.Entries.Count != Files.Count)
            throw new InvalidDataException("The Claude Code package must contain both executables.");
        foreach (var (name, size) in Files)
        {
            var entry = zip.GetEntry(name);
            if (entry is null || entry.Length != size)
                throw new InvalidDataException("The Claude Code package contains an unexpected executable.");
            await using var input = entry.Open();
            await using var output = new FileStream(Path.Combine(payload, name), FileMode.CreateNew,
                FileAccess.Write, FileShare.None, 128 * 1024, FileOptions.Asynchronous);
            await input.CopyToAsync(output, cancellation).ConfigureAwait(false);
            if (output.Length != size) throw new InvalidDataException("A packaged Claude Code file is incomplete.");
        }
    }
}
