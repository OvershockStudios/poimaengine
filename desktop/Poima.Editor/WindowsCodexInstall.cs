// SPDX-License-Identifier: Apache-2.0
using System.Formats.Tar;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace Poima.Editor;

internal static class WindowsCodexInstall
{
    // Keep this release, its publisher digest and its layout bounds together.
    public const string Version = "0.171.0";
    internal const long ArchiveBytes = 214_982_795;
    private const long PayloadBytes = 625_178_849;
    private const int PayloadFiles = 46;
    internal const string ArchiveSha256 = "669941eff23dd8470fbf85d919a03d80c4e3d35b441e3fcd361c0f8630945be8";
    internal const string PackageUrl = $"https://releases.openai.com/codex/releases/{Version}/codex-package-x86_64-pc-windows-msvc.tar.gz";
    private const string Marker = ".poima-codex-installed";

    public static bool Supported => OperatingSystem.IsWindows() && RuntimeInformation.OSArchitecture == Architecture.X64;
    public static string Root => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Poima", "agents", "codex");

    public static string? Executable(string root)
    {
        var folder = Path.Combine(root, Version);
        var receipt = new FileInfo(Path.Combine(folder, Marker));
        var executable = new FileInfo(Path.Combine(folder, "bin", "codex.exe"));
        try
        {
            if (!SafeDirectory(root) || !SafeDirectory(folder) || !SafeDirectory(Path.Combine(folder, "bin")) ||
                !receipt.Exists || receipt.Length != ArchiveSha256.Length || receipt.Attributes.HasFlag(FileAttributes.ReparsePoint) ||
                File.ReadAllText(receipt.FullName) != ArchiveSha256 || !executable.Exists || executable.Length != 407_421_232 ||
                executable.Attributes.HasFlag(FileAttributes.ReparsePoint)) return null;
            return executable.FullName;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    public static async Task<string> InstallAsync(HttpClient http, string root, IProgress<int>? progress, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        Directory.CreateDirectory(root);
        if (!SafeDirectory(root)) throw new IOException("Choose a local Poima installation folder.");
        var lockPath = Path.Combine(root, ".install.lock");
        if (File.Exists(lockPath) && File.GetAttributes(lockPath).HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("The Codex installation lock is unavailable.");
        await using var installLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (Executable(root) is { } existing) return existing;

        var destination = Path.Combine(root, Version);
        if (Directory.Exists(destination) || File.Exists(destination))
            throw new IOException("An incomplete Codex folder is already present. Open the setup guide or select an existing Codex executable.");
        var staging = Path.Combine(root, ".install-" + Guid.NewGuid().ToString("N"));
        var payload = Directory.CreateDirectory(Path.Combine(staging, "payload")).FullName;
        try
        {
            var archivePath = Path.Combine(staging, "codex.tar.gz");
            await using (var archive = new FileStream(archivePath, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None,
                128 * 1024, FileOptions.Asynchronous))
            {
                await DownloadAsync(http, archive, progress, cancellation).ConfigureAwait(false);
                archive.Position = 0;
                await UnpackAsync(archive, payload, cancellation).ConfigureAwait(false);
            }
            await File.WriteAllTextAsync(Path.Combine(payload, Marker), ArchiveSha256, cancellation).ConfigureAwait(false);
            cancellation.ThrowIfCancellationRequested();
            Directory.Move(payload, destination);
            return Path.Combine(destination, "bin", "codex.exe");
        }
        finally { Directory.Delete(staging, recursive: true); }
    }

    private static bool SafeDirectory(string path) => Directory.Exists(path) && !File.GetAttributes(path).HasFlag(FileAttributes.ReparsePoint);

    private static async Task DownloadAsync(HttpClient http, Stream archive, IProgress<int>? progress, CancellationToken cancellation)
    {
        using var response = await http.GetAsync(PackageUrl, HttpCompletionOption.ResponseHeadersRead, cancellation).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is { } expected && expected != ArchiveBytes)
            throw new InvalidDataException("The Codex download has an unexpected size.");
        await using var input = await response.Content.ReadAsStreamAsync(cancellation).ConfigureAwait(false);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[128 * 1024];
        long received = 0;
        int read, reported = -1;
        while ((read = await input.ReadAsync(buffer, cancellation).ConfigureAwait(false)) != 0)
        {
            received += read;
            if (received > ArchiveBytes) throw new InvalidDataException("The Codex download exceeded its published size.");
            hash.AppendData(buffer.AsSpan(0, read));
            await archive.WriteAsync(buffer.AsMemory(0, read), cancellation).ConfigureAwait(false);
            var percent = (int)(received * 100 / ArchiveBytes);
            if (percent == reported) continue;
            progress?.Report(percent);
            reported = percent;
        }
        if (received != ArchiveBytes || Convert.ToHexStringLower(hash.GetHashAndReset()) != ArchiveSha256)
            throw new InvalidDataException("The Codex download did not match OpenAI’s published checksum.");
    }

    private static async Task UnpackAsync(Stream archive, string payload, CancellationToken cancellation)
    {
        using var compressed = new GZipStream(archive, CompressionMode.Decompress, leaveOpen: true);
        using var tar = new TarReader(compressed, leaveOpen: true);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        while (await tar.GetNextEntryAsync(copyData: false, cancellation).ConfigureAwait(false) is { } entry)
        {
            if (entry.EntryType == TarEntryType.Directory) continue;
            if (entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile) ||
                entry.DataStream is null || !SafeName(entry.Name) || !names.Add(entry.Name) || names.Count > PayloadFiles ||
                entry.Length < 0 || entry.Length > PayloadBytes - total)
                throw new InvalidDataException("The Codex package contains an unexpected entry.");
            total += entry.Length;
            var file = Path.Combine(payload, entry.Name.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            await using var output = new FileStream(file, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                128 * 1024, FileOptions.Asynchronous);
            await entry.DataStream.CopyToAsync(output, cancellation).ConfigureAwait(false);
            if (output.Length != entry.Length) throw new InvalidDataException("A packaged Codex file is incomplete.");
        }
        if (names.Count != PayloadFiles || total != PayloadBytes ||
            !new[] { "codex-package.json", "bin/codex.exe", "bin/codex-exec-server.exe", "codex-path/rg.exe",
                "codex-resources/codex-windows-sandbox-setup.exe" }.All(names.Contains))
            throw new InvalidDataException("The Codex package is incomplete.");
    }

    private static bool SafeName(string name)
    {
        if (name == "codex-package.json") return true;
        var parts = name.Split('/');
        return name.Length <= 200 && parts.Length >= 2 && (parts[0] is "bin" or "codex-path" or "codex-resources") &&
            parts.All(part => part.Length > 0 && char.IsAsciiLetterOrDigit(part[0]) && part[^1] != '.' &&
                part.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-' or '.'));
    }
}
