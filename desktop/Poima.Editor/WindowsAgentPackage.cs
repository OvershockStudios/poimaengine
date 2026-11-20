// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace Poima.Editor;

internal sealed class WindowsAgentPackage(
    string name, string directoryName, string version, string url, long downloadBytes, string checksum, string marker,
    string executablePath, IReadOnlyDictionary<string, long> requiredFiles,
    Func<Stream, string, CancellationToken, Task>? unpack,
    HashAlgorithmName algorithm = default)
{
    public string Name => name;
    public string Version => version;
    public static bool Supported => OperatingSystem.IsWindows() && RuntimeInformation.OSArchitecture == Architecture.X64;
    public string Root => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Poima", "agents", directoryName);

    public string? Executable(string root)
    {
        var folder = Path.Combine(root, version);
        try
        {
            if (!SafeDirectory(root) || !SafeDirectory(folder) ||
                !RegularFile(folder, marker, checksum.Length) || File.ReadAllText(Path.Combine(folder, marker)) != checksum ||
                !requiredFiles.All(file => RegularFile(folder, file.Key, file.Value))) return null;
            return Path.Combine(folder, executablePath);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    public async Task<string> InstallAsync(HttpClient http, string root, IProgress<int>? progress, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        Directory.CreateDirectory(root);
        if (!SafeDirectory(root)) throw new IOException("Choose a local Poima installation folder.");
        var lockPath = Path.Combine(root, ".install.lock");
        if (File.Exists(lockPath) && File.GetAttributes(lockPath).HasFlag(FileAttributes.ReparsePoint))
            throw new IOException($"The {name} installation lock is unavailable.");
        await using var installLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (Executable(root) is { } existing) return existing;

        var destination = Path.Combine(root, version);
        if (Directory.Exists(destination) || File.Exists(destination))
            throw new IOException($"An incomplete {name} folder is already present. Open the setup guide or choose an existing executable.");
        var staging = Path.Combine(root, ".install-" + Guid.NewGuid().ToString("N"));
        var payload = Directory.CreateDirectory(Path.Combine(staging, "payload")).FullName;
        try
        {
            var download = Path.Combine(staging, "download");
            await using (var archive = new FileStream(download, FileMode.CreateNew,
                FileAccess.ReadWrite, FileShare.None, 128 * 1024, FileOptions.Asynchronous))
            {
                await DownloadAsync(http, archive, progress, cancellation).ConfigureAwait(false);
                if (unpack is not null)
                {
                    archive.Position = 0;
                    await unpack(archive, payload, cancellation).ConfigureAwait(false);
                }
            }
            if (unpack is null) File.Move(download, Path.Combine(payload, executablePath));
            await File.WriteAllTextAsync(Path.Combine(payload, marker), checksum, cancellation).ConfigureAwait(false);
            cancellation.ThrowIfCancellationRequested();
            Directory.Move(payload, destination);
            return Path.Combine(destination, executablePath);
        }
        finally { Directory.Delete(staging, recursive: true); }
    }

    private static bool SafeDirectory(string path) =>
        Directory.Exists(path) && !File.GetAttributes(path).HasFlag(FileAttributes.ReparsePoint);

    private static bool RegularFile(string folder, string name, long size)
    {
        for (var parent = Path.GetDirectoryName(name); !string.IsNullOrEmpty(parent); parent = Path.GetDirectoryName(parent))
            if (!SafeDirectory(Path.Combine(folder, parent))) return false;
        var file = new FileInfo(Path.Combine(folder, name));
        return file.Exists && file.Length == size && !file.Attributes.HasFlag(FileAttributes.ReparsePoint);
    }

    private async Task DownloadAsync(HttpClient http, Stream archive, IProgress<int>? progress, CancellationToken cancellation)
    {
        using var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellation).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is { } expected && expected != downloadBytes)
            throw new InvalidDataException($"The {name} download has an unexpected size.");
        await using var input = await response.Content.ReadAsStreamAsync(cancellation).ConfigureAwait(false);
        using var hash = IncrementalHash.CreateHash(algorithm == default ? HashAlgorithmName.SHA256 : algorithm);
        var buffer = new byte[128 * 1024];
        long received = 0;
        int read, reported = -1;
        while ((read = await input.ReadAsync(buffer, cancellation).ConfigureAwait(false)) != 0)
        {
            received += read;
            if (received > downloadBytes) throw new InvalidDataException($"The {name} download exceeded its published size.");
            hash.AppendData(buffer.AsSpan(0, read));
            await archive.WriteAsync(buffer.AsMemory(0, read), cancellation).ConfigureAwait(false);
            var percent = (int)(received * 100 / downloadBytes);
            if (percent == reported) continue;
            progress?.Report(percent);
            reported = percent;
        }
        if (received != downloadBytes || Convert.ToHexStringLower(hash.GetHashAndReset()) != checksum)
            throw new InvalidDataException($"The {name} download did not match the pinned checksum.");
    }
}
