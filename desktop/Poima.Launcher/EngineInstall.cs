// SPDX-License-Identifier: Apache-2.0
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;

namespace Poima.Launcher;

internal sealed record InstallState(string? Active = null, string? Previous = null, string? HeldUpdate = null);
internal sealed record InstallResult(string? Version, bool Downloaded, bool Held = false);
internal sealed record PackageFile(string Path, long Bytes, string Sha256);

internal sealed record PackageInventory(string Version, IReadOnlyDictionary<string, PackageFile> Files)
{
    private static readonly string[] Required = ["Poima.Editor.exe", "poima.exe", "poima-desktop-build.json", "START.md"];

    public static PackageInventory Read(byte[] bytes, EngineRelease release)
    {
        using var document = JsonDocument.Parse(bytes);
        var top = document.RootElement;
        var archive = top.GetProperty("archive");
        if (top.GetProperty("format").GetString() != "poima.windows-release" ||
            top.GetProperty("version").GetInt32() != 1 ||
            top.GetProperty("engine_version").GetString() != release.Version ||
            archive.GetProperty("name").GetString() != ReleaseFeed.Folder(release.Version) + ".zip" ||
            archive.GetProperty("bytes").GetInt64() != release.Archive.Bytes ||
            archive.GetProperty("sha256").GetString() != release.Archive.Sha256)
            throw new InvalidDataException("The inventory does not describe the selected Windows release.");

        var entries = top.GetProperty("files");
        if (entries.GetArrayLength() is < 4 or > 10000)
            throw new InvalidDataException("The release inventory has an unsupported number of files.");
        var files = new Dictionary<string, PackageFile>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var entry in entries.EnumerateArray())
        {
            var path = entry.GetProperty("path").GetString() ?? "";
            var size = entry.GetProperty("bytes").GetInt64();
            var sha = entry.GetProperty("sha256").GetString() ?? "";
            if (!SafePath(path) || size is < 0 or > ReleaseFeed.MaxArchiveBytes ||
                !ReleaseFeed.IsSha256(sha) || !files.TryAdd(path, new(path, size, sha)))
                throw new InvalidDataException("The release inventory contains an invalid or repeated file.");
            total += size;
            if (total > 8 * ReleaseFeed.MaxArchiveBytes)
                throw new InvalidDataException("The expanded package exceeds the launcher's size limit.");
        }
        if (Required.Any(path => !files.TryGetValue(path, out var item) || item.Path != path))
            throw new InvalidDataException("Required engine files are missing from the release inventory.");
        return new(release.Version, files);
    }

    internal static bool SafePath(string path)
    {
        if (path.Length is < 1 or > 1024) return false;
        foreach (var segment in path.Split('/'))
        {
            if (segment.Length is < 1 or > 255 || segment[^1] is '.' or ' ' ||
                segment.Any(c => c < ' ' || "<>:\"\\|?*".Contains(c))) return false;
            var name = segment.Split('.')[0].TrimEnd().ToUpperInvariant();
            if (name is "CON" or "PRN" or "AUX" or "NUL" or "CONIN$" or "CONOUT$" ||
                name.Length == 4 && (name.StartsWith("COM", StringComparison.Ordinal) ||
                    name.StartsWith("LPT", StringComparison.Ordinal)) && "123456789¹²³".Contains(name[3]))
                return false;
        }
        return !path.Equals(".poima-install.json", StringComparison.OrdinalIgnoreCase);
    }

    public async Task ExtractAsync(string archive, string destination, CancellationToken cancel)
    {
        using var package = ZipFile.OpenRead(archive);
        if (package.Entries.Count != Files.Count) throw new InvalidDataException("The archive inventory differs from the published inventory.");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var prefix = ReleaseFeed.Folder(Version) + "/";
        foreach (var member in package.Entries)
        {
            cancel.ThrowIfCancellationRequested();
            var name = member.FullName;
            var relative = name.StartsWith(prefix, StringComparison.Ordinal) ? name[prefix.Length..] : "";
            var type = (member.ExternalAttributes >> 16) & 0xf000;
            var nonFile = member.ExternalAttributes & (int)(FileAttributes.Directory | FileAttributes.ReparsePoint);
            if (!Files.TryGetValue(relative, out var file) || relative != file.Path || !seen.Add(relative) ||
                type is not (0 or 0x8000) || nonFile != 0 || member.Length != file.Bytes)
                throw new InvalidDataException("The archive contains an unexpected file or path.");

            var outputPath = Path.Combine(destination, relative.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            await using var output = new FileStream(outputPath, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                128 * 1024, FileOptions.Asynchronous);
            await using var input = member.Open();
            await ReleaseFeed.CopyVerifiedAsync(input, output, file.Bytes, file.Sha256, null, cancel);
        }
    }
}

internal sealed class EngineInstall(string root, ReleaseFeed feed)
{
    private sealed record Receipt(string Version, long EditorBytes, string EditorSha256);
    private string Versions => Path.Combine(root, "versions");
    private string StatePath => Path.Combine(root, ".poima-launcher.json");

    private string InstallFolder(string version)
    {
        if (EngineVersion.Parse(version) is null) throw new InvalidDataException("The installed version is invalid.");
        return Path.Combine(Versions, ReleaseFeed.Folder(version));
    }

    public InstallState State() => File.Exists(StatePath)
        ? ReadSmall<InstallState>(StatePath)
        : new();

    public string? Executable(string? version)
    {
        if (version is null) return null;
        var folder = InstallFolder(version);
        if (!Directory.Exists(folder)) return null;
        CheckDirectory(folder);
        var marker = Path.Combine(folder, ".poima-install.json");
        if (!File.Exists(marker)) return null;
        var receipt = ReadSmall<Receipt>(marker);
        if (receipt.Version != version || !ReleaseFeed.IsSha256(receipt.EditorSha256)) return null;
        var editor = Path.Combine(folder, "Poima.Editor.exe");
        var info = new FileInfo(editor);
        if (!info.Exists || info.Length != receipt.EditorBytes || info.Attributes.HasFlag(FileAttributes.ReparsePoint)) return null;
        using var stream = File.OpenRead(editor);
        return ReleaseFeed.Hex(SHA256.HashData(stream)) == receipt.EditorSha256 ? editor : null;
    }

    public async Task<InstallResult> UpdateAsync(ReleaseChannel channel, bool automatic, IProgress<string>? status,
        IProgress<double>? progress, CancellationToken cancel)
    {
        Directory.CreateDirectory(root);
        CheckDirectory(root);
        var lockPath = Path.Combine(root, ".poima-install.lock");
        RejectLink(lockPath);
        await using var installLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        var state = State();
        status?.Report("Checking for a Windows release…");
        using var check = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        check.CancelAfter(TimeSpan.FromSeconds(45));
        EngineRelease? chosen;
        try { chosen = await feed.LatestAsync(channel, check.Token); }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested)
        {
            throw new TimeoutException("The release check timed out. You can still open an installed engine.");
        }
        if (chosen is null) return new(null, false);
        if (automatic && state.HeldUpdate == chosen.Version) return new(chosen.Version, false, true);
        var available = Executable(chosen.Version) is not null;
        if (!available)
        {
            status?.Report($"Downloading Poima {chosen.Version}…");
            await DownloadAsync(chosen, status, progress, cancel);
        }
        cancel.ThrowIfCancellationRequested();
        if (state.Active != chosen.Version)
            SaveState(new(chosen.Version, Executable(state.Active) is not null ? state.Active : state.Previous));
        return new(chosen.Version, !available);
    }

    public string Rollback()
    {
        var lockPath = Path.Combine(root, ".poima-install.lock");
        CheckDirectory(root);
        RejectLink(lockPath);
        using var installLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        var state = State();
        if (Executable(state.Previous) is null) throw new InvalidOperationException("The previous engine is unavailable.");
        SaveState(new(state.Previous, state.Active, SelectHeldUpdate(state)));
        return state.Previous!;
    }

    private static string? SelectHeldUpdate(InstallState state)
    {
        var active = EngineVersion.Parse(state.Active ?? "");
        var prior = EngineVersion.Parse(state.Previous ?? "");
        return active is not null && active.CompareTo(prior) > 0 ? state.Active : null;
    }

    private async Task DownloadAsync(EngineRelease release, IProgress<string>? status, IProgress<double>? progress,
        CancellationToken cancel)
    {
        Directory.CreateDirectory(Versions);
        CheckDirectory(Versions);
        var target = InstallFolder(release.Version);
        if (Directory.Exists(target) || File.Exists(target))
            throw new IOException("An incomplete or modified installation occupies the selected version's folder.");
        var inventory = PackageInventory.Read(await feed.InventoryAsync(release.Inventory, cancel), release);
        var staging = Path.Combine(root, ".poima-staging");
        Directory.CreateDirectory(staging);
        CheckDirectory(staging);
        var work = Directory.CreateDirectory(Path.Combine(staging, Guid.NewGuid().ToString("N"))).FullName;
        try
        {
            var archive = Path.Combine(work, "release.zip");
            await feed.DownloadAsync(release.Archive, archive, progress, cancel);
            status?.Report($"Verifying Poima {release.Version}…");
            var extracted = Directory.CreateDirectory(Path.Combine(work, "engine")).FullName;
            await inventory.ExtractAsync(archive, extracted, cancel);
            var editor = inventory.Files["Poima.Editor.exe"];
            WriteNew(Path.Combine(extracted, ".poima-install.json"), new Receipt(release.Version, editor.Bytes, editor.Sha256));
            cancel.ThrowIfCancellationRequested();
            Directory.Move(extracted, target);
        }
        finally
        {
            Directory.Delete(work, recursive: true);
        }
    }

    private void SaveState(InstallState state)
    {
        var temporary = Path.Combine(root, $".poima-state-{Guid.NewGuid():N}.tmp");
        try
        {
            WriteNew(temporary, state);
            RejectLink(StatePath);
            File.Move(temporary, StatePath, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    private static void WriteNew<T>(string path, T value)
    {
        using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        JsonSerializer.Serialize(stream, value);
        stream.Flush(flushToDisk: true);
    }

    private static T ReadSmall<T>(string path)
    {
        RejectLink(path);
        using var stream = File.OpenRead(path);
        if (stream.Length > 16 * 1024) throw new InvalidDataException("The install record is too large.");
        return JsonSerializer.Deserialize<T>(stream) ?? throw new InvalidDataException("The install record is empty.");
    }

    private static void CheckDirectory(string path)
    {
        for (var directory = new DirectoryInfo(path); directory is not null; directory = directory.Parent)
            RejectLink(directory.FullName);
    }

    private static void RejectLink(string path)
    {
        try
        {
            if (File.GetAttributes(path).HasFlag(FileAttributes.ReparsePoint))
                throw new IOException("Poima's installation paths must use ordinary files and folders.");
        }
        catch (FileNotFoundException) { }
        catch (DirectoryNotFoundException) { }
    }
}
